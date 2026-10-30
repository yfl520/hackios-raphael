/* Raphael input telemetry through the resident firmware's ADC protocol.
 * No direct ADC/charger writes, calibration requests, thresholds or limits.
 * ABI checked against AdcDxe.raphael.efi revision 0x10004: table 0x82e0,
 * GetChannel 0x1454, AdcRead 0x14bc. Public interface provenance is recorded
 * in ../firmware-input-research/sources.json. Run only on the UEFI core.
 */
typedef struct { uint32_t device, channel; } raphael_adc_channel;
typedef struct { int32_t status, physical, percent, microvolts, code; } raphael_adc_result;
typedef struct {
	uint64_t revision;
	efi_status (EFIAPI *get_channel)(const char *, uint32_t, raphael_adc_channel *);
	efi_status (EFIAPI *read)(raphael_adc_channel *, raphael_adc_result *);
	void *recalibrate, *synchronous_read, *set_threshold, *force_battery_id;
} raphael_adc_protocol;
_Static_assert(sizeof(raphael_adc_channel) == 8, "ADC channel ABI");
_Static_assert(sizeof(raphael_adc_result) == 20, "ADC result ABI");
_Static_assert(__builtin_offsetof(raphael_adc_protocol, read) == 16, "ADC read ABI");

#define RAPHAEL_POWER_ROWS 128
static raphael_adc_protocol *raphael_adc;
static efi_status power_protocol_status;
static struct {
	const char *name;
	raphael_adc_channel channel;
	efi_status lookup_status;
	int found;
} power_channels[] = {
	{ .name = "USB_IN" }, { .name = "USB_IN_I" }, { .name = "VCHG" },
};
static struct power_row {
	uint64_t milliseconds, duration_ms, status[3];
	raphael_adc_result result[3];
	uint32_t fg_polls, fg_flags, valid_pair;
	int32_t soc, battery_mv, battery_ma, temperature_c, input_mv, input_ma;
	uint32_t current_kind;
} power_rows[RAPHAEL_POWER_ROWS];
static uint64_t power_began, power_samples;
static int power_requested, power_started;
/* 1: native mA; 2: PM8150B buck current-sense mV (200mV/A). */
static unsigned power_current_kind;

static uint64_t
power_elapsed_ms(uint64_t ticks)
{
	uint64_t freq = READ_SYSREG(CNTFRQ_EL0);
	return (ticks / freq) * 1000 + (ticks % freq) * 1000 / freq;
}

static void
raphael_power_start(void)
{
	static const efi_guid guid = {0x61740cc4,0x916f,0x4471,{0xb0,0xdc,0,0xc6,0xf4,0x3a,0x7f,0xc6}};
	if (!power_requested || power_started || PLAT != &platform_raphael) return;
	power_started = 1;
	power_began = READ_SYSREG(CNTVCT_EL0);
	power_protocol_status = BS->locate_protocol(&guid, NULL, (void **)&raphael_adc);
	if (EFI_ERROR(power_protocol_status) || !raphael_adc || raphael_adc->revision != 0x10004 ||
	    !raphael_adc->get_channel || !raphael_adc->read) {
		raphael_adc = NULL;
		return;
	}
	for (unsigned i = 0; i < 3; i++) {
		uint32_t length = 1;
		for (const char *p = power_channels[i].name; *p; p++) length++;
		power_channels[i].lookup_status = raphael_adc->get_channel(power_channels[i].name,
		    length, &power_channels[i].channel);
		if (i == 1 && EFI_ERROR(power_channels[i].lookup_status)) {
			/* Native PM855B BSP exposes the sense voltage, not the generic
			 * current channel. This exact string is in DALSYSDxe.raphael.efi. */
			power_channels[i].name = "USB_IN_I_MV";
			power_channels[i].lookup_status = raphael_adc->get_channel("USB_IN_I_MV",
			    sizeof("USB_IN_I_MV"), &power_channels[i].channel);
			power_current_kind = 2;
		} else if (i == 1) power_current_kind = 1;
		power_channels[i].found = !EFI_ERROR(power_channels[i].lookup_status);
	}
}

static void
raphael_power_poll(void)
{
	if (!power_requested || !power_started || !raphael_adc) return;
	/* Called after the normal FG query, on the same firmware core; never
	 * perform an ADC conversion from XNU concurrently with that firmware. */
	if (charger_shared.polls % 3) return;
	struct power_row row;
	memset(&row, 0, sizeof(row));
	uint64_t start = READ_SYSREG(CNTVCT_EL0);
	row.milliseconds = power_elapsed_ms(start - power_began);
	row.fg_polls = charger_shared.polls;
	row.fg_flags = charger_shared.flags;
	row.soc = charger_shared.soc_percent;
	row.battery_mv = charger_shared.voltage_mv;
	row.battery_ma = charger_shared.current_ma;
	row.temperature_c = charger_shared.temperature_c;
	for (unsigned i = 0; i < 3; i++) {
		row.status[i] = (1ULL << 63) | 3;
		if (power_channels[i].found)
			row.status[i] = raphael_adc->read(&power_channels[i].channel, &row.result[i]);
	}
	row.duration_ms = power_elapsed_ms(READ_SYSREG(CNTVCT_EL0) - start);
	row.input_mv = row.result[0].physical;
	row.current_kind = power_current_kind;
	int64_t scaled_ma = row.result[1].physical;
	if (power_current_kind == 2) scaled_ma *= 5;
	row.input_ma = scaled_ma >= 0 && scaled_ma <= 6000 ? (int32_t)scaled_ma : -1;
	/* Generic USB_IN_I already reports mA. Raphael's USB_IN_I_MV instead
	 * reports sensor mV: PM8150B sink/buck conversion is 0.2V/A (5mA/mV).
	 * This telemetry is for the attached charging source, never OTG tests.

	 * Leave invalid/stale/timeout samples raw; do not fabricate watts. */
	row.valid_pair = !EFI_ERROR(row.status[0]) && !EFI_ERROR(row.status[1]) &&
	    row.result[0].status == 1 && row.result[1].status == 1 &&
	    row.result[0].physical >= 2500 && row.result[0].physical <= 22000 &&
	    row.input_ma >= 0 && row.input_ma <= 6000 &&
	    row.duration_ms <= 1000 && (row.fg_flags & LOADER_BATTERY_CHARGER_PRESENT);
	power_rows[power_samples++ % RAPHAEL_POWER_ROWS] = row;
}

static uint64_t
power_text(char *out, uint64_t capacity, uint64_t at, const char *text)
{
	while (*text && at < capacity) out[at++] = *text++;
	return at;
}

static uint64_t
power_number(char *out, uint64_t capacity, uint64_t at, uint64_t value, int negative)
{
	char digits[24]; unsigned count = 0;
	if (negative) at = power_text(out, capacity, at, "-");
	do { digits[count++] = '0' + value % 10; value /= 10; } while (value);
	while (count && at < capacity) out[at++] = digits[--count];
	return power_text(out, capacity, at, "\t");
}

static uint64_t
raphael_power_append(char *out, uint64_t capacity, uint64_t at)
{
	if (!power_requested || !power_started) return at;
	at = power_text(out, capacity, at, "\n== RAPHAEL INPUT ADC v2; raw firmware units; kind1=mA, kind2=sensor_mV; sequential sink samples ==\nprotocol_status\t");
	at = power_number(out, capacity, at, power_protocol_status, 0);
	at = power_text(out, capacity, at, raphael_adc ? "accepted\n" : "unavailable-or-ABI-rejected\n");
	if (!raphael_adc) return at;
	for (unsigned i = 0; i < 3; i++) {
		at = power_text(out, capacity, at, power_channels[i].name);
		at = power_text(out, capacity, at, "\tlookup\t");
		at = power_number(out, capacity, at, power_channels[i].lookup_status, 0);
		at = power_number(out, capacity, at, power_channels[i].channel.device, 0);
		at = power_number(out, capacity, at, power_channels[i].channel.channel, 0);
		at = power_text(out, capacity, at, "\n");
	}
	at = power_text(out, capacity, at, "ms\tduration_ms\tfg_polls\tfg_flags\tsoc\tbattery_mv\tbattery_raw_ma\ttemp_c\tpair_valid\tcurrent_kind\tUSB_IN_efi\tUSB_IN_status\tUSB_IN_mv\tUSB_IN_uv\tUSB_IN_code\tUSB_IN_I_efi\tUSB_IN_I_status\tUSB_IN_I_physical\tUSB_IN_I_uv\tUSB_IN_I_code\tVCHG_efi\tVCHG_status\tVCHG_mv\tVCHG_uv\tVCHG_code\tinput_mv\tinput_ma\n");
	uint64_t first = power_samples > RAPHAEL_POWER_ROWS ? power_samples - RAPHAEL_POWER_ROWS : 0;
	for (uint64_t n = first; n < power_samples; n++) {
		const struct power_row *r = &power_rows[n % RAPHAEL_POWER_ROWS];
		at = power_number(out, capacity, at, r->milliseconds, 0);
		at = power_number(out, capacity, at, r->duration_ms, 0);
		at = power_number(out, capacity, at, r->fg_polls, 0);
		at = power_number(out, capacity, at, r->fg_flags, 0);
		int32_t values[] = {r->soc,r->battery_mv,r->battery_ma,r->temperature_c,(int32_t)r->valid_pair};
		for (unsigned i = 0; i < 5; i++) {
			int64_t v = values[i];
			at = power_number(out, capacity, at, v < 0 ? (uint64_t)-v : (uint64_t)v, v < 0);
		}
		at = power_number(out, capacity, at, r->current_kind, 0);
		for (unsigned i = 0; i < 3; i++) {
			at = power_number(out, capacity, at, r->status[i], 0);
			const raphael_adc_result *a = &r->result[i];
			int32_t raw[] = {a->status,a->physical,a->microvolts,a->code};
			for (unsigned j = 0; j < 4; j++) {
				int64_t v = raw[j];
				at = power_number(out, capacity, at, v < 0 ? (uint64_t)-v : (uint64_t)v, v < 0);
			}
		}
		int64_t mv = r->input_mv, ma = r->input_ma;
		at = power_number(out, capacity, at, mv < 0 ? (uint64_t)-mv : (uint64_t)mv, mv < 0);
		at = power_number(out, capacity, at, ma < 0 ? (uint64_t)-ma : (uint64_t)ma, ma < 0);
		at = power_text(out, capacity, at, "\n");
	}
	return power_text(out, capacity, at, "== END INPUT ADC ==\n");
}
