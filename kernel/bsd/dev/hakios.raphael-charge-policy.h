/* Bounded adaptation of pinned SM8150 smblib_handle_usb_current and
 * smblib_set_icl_current. Never relabel FLOAT as DCP. FLOAT is eligible only
 * after slow-plugin timeout, no enumeration, and a valid ordinary Type-C sink
 * attachment. Runtime must additionally require 60s of continuous stability.
 * Only a <=500mA inherited ICL may be adopted, with a 1000mA ceiling.
 */
/* Saved live-DT sun/gyu profiles use the normal range through450dC;
 * above450dC stock also lowers FV. Integer cache cannot represent fractions,
 * so reject45C and above; permit41..44C at this stage's <=5V/1A cap. */
static int raphael_charge_temperature_ok(int c)
{
    return c >= 15 && c < 45;
}

enum { RAPHAEL_CHARGE_NONE, RAPHAEL_CHARGE_DEDICATED, RAPHAEL_CHARGE_FLOAT };

static unsigned
raphael_charge_class(unsigned plugin, unsigned apsd, unsigned result,
    unsigned thermal, unsigned enable, unsigned pause, unsigned aicl,
    int valid_fresh_gauge, int temperature, int voltage,
    unsigned suspend, unsigned otg, unsigned options,
    int typec_valid, unsigned typec_sink, unsigned typec_misc, unsigned legacy)
{
    if ((plugin & 0x1f) != 0x10 || !(apsd & 1) || (thermal & 0x3f) ||
        !(enable & 1) || (pause & 1) || !(aicl & 4) ||
        !valid_fresh_gauge || !raphael_charge_temperature_ok(temperature) ||
        voltage < 3200 || voltage > 4300 || (suspend & 1) ||
        (otg & 1) || !(options & 8)) return RAPHAEL_CHARGE_NONE;
    /* An enumerated host is never adopted, even if APSD classification is
     * inconsistent with enumeration. SDP, mixed and unknown results reject. */
    if (apsd & 0x10) return RAPHAEL_CHARGE_NONE;
    if (result == 0x02 || result == 0x04 || result == 0x08 ||
        result == 0x28 || result == 0x48) return RAPHAEL_CHARGE_DEDICATED;
    if (result != 0x10 || !(apsd & 0x20) || !typec_valid ||
        !(typec_misc & 1) || !(typec_misc & 8) || (typec_misc & 0xd0) ||
        (legacy & 1)) return RAPHAEL_CHARGE_NONE;
    unsigned rp = typec_sink & 0x7f;
    if (rp != 8 && rp != 4 && rp != 2) return RAPHAEL_CHARGE_NONE;
    return RAPHAEL_CHARGE_FLOAT;
}

/* Pure state transition helper: an ineligible or changed source restarts the
 * settling interval. Losing eligibility after adoption requires restoration.
 * Used by the runtime, and by transition tests, rather than duplicated logic.
 */
struct raphael_charge_gate {
    unsigned source;
    unsigned long long since;
};

static int
raphael_charge_settled(struct raphael_charge_gate *gate, unsigned source,
    unsigned long long now_ns)
{
    if (!source) { gate->source = 0; gate->since = 0; return 0; }
    if (gate->source != source || now_ns < gate->since) {
        gate->source = source;
        gate->since = now_ns;
        return 0;
    }
    unsigned long long wait_ns = source == RAPHAEL_CHARGE_FLOAT ?
        60000000000ULL : 15000000000ULL;
    return now_ns - gate->since >= wait_ns;
}
