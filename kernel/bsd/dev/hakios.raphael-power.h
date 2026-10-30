/* Raphael IMX586 regulator request lifecycle; no PMIC/MMIO addresses here.
 * Source: live FDT and pinned SM8150 rpmh-regulator.c. Values are this DRV's
 * requests, not physical aggregated voltage. The adapter serializes all calls.
 * An attempted write is dirty even when its completion or readback fails.
 */
struct raphael_power_ops {
	bool (*read)(void *, unsigned, unsigned, uint32_t *);
	bool (*write)(void *, unsigned, unsigned, uint32_t);
};

struct raphael_power_state {
	uint32_t saved[4][3];
	unsigned dirty[4];
	bool active, failed;
};

static const struct {
	const char *name;
	uint32_t minimum_mv, maximum_mv, mode;
	bool set_mode;
} raphael_power_rails[] = {
	{ "ldoa17", 3100, 3100, 7, true },
	{ "bobc1", 3008, 4000, 2, true },
	{ "smpa5", 1904, 2040, 0, false },
	{ "smpc8", 1352, 1352, 6, true },
};

static bool
raphael_power_begin(struct raphael_power_state *s, const struct raphael_power_ops *ops, void *ctx)
{
	if (!s || !ops || !ops->read || !ops->write || s->active) return false;
	for (unsigned rail = 0; rail < 4; rail++) {
		s->dirty[rail] = 0;
		for (unsigned field = 0; field < 3; field++)
			if (!ops->read(ctx, rail, field, &s->saved[rail][field])) return false;
		if (s->saved[rail][0] > raphael_power_rails[rail].maximum_mv ||
		    s->saved[rail][1] > 1 || s->saved[rail][2] > 7 ||
		    (s->saved[rail][1] && s->saved[rail][0] < raphael_power_rails[rail].minimum_mv)) return false;
	}
	s->failed = false;
	s->active = true;
	return true;
}

static bool
raphael_power_put(struct raphael_power_state *s, const struct raphael_power_ops *ops, void *ctx,
    unsigned rail, unsigned field, uint32_t value)
{
	uint32_t back = 0;
	/* Record before attempting the command: it may take effect and then fail. */
	s->dirty[rail] |= 1u << field;
	if (!ops->write(ctx, rail, field, value) || !ops->read(ctx, rail, field, &back) || back != value) {
		s->failed = true;
		return false;
	}
	return true;
}

static bool
raphael_power_enable(struct raphael_power_state *s, const struct raphael_power_ops *ops, void *ctx, unsigned rail)
{
	if (!s || !s->active || s->failed || rail >= 4 || s->dirty[rail]) return false;
	uint32_t voltage = s->saved[rail][0];
	if (voltage < raphael_power_rails[rail].minimum_mv) voltage = raphael_power_rails[rail].minimum_mv;
	/* Never lower an existing request on the shared rail. No mode table on S5. */
	if (voltage != s->saved[rail][0] && !raphael_power_put(s, ops, ctx, rail, 0, voltage)) return false;
	if (raphael_power_rails[rail].set_mode && s->saved[rail][2] < raphael_power_rails[rail].mode &&
	    !raphael_power_put(s, ops, ctx, rail, 2, raphael_power_rails[rail].mode)) return false;
	if (!s->saved[rail][1] && !raphael_power_put(s, ops, ctx, rail, 1, 1)) return false;
	return true;
}

static bool
raphael_power_restore(struct raphael_power_state *s, const struct raphael_power_ops *ops, void *ctx, unsigned rail)
{
	if (!s || !s->active || rail >= 4) return false;
	/* If originally disabled, first remove our enable request before reducing
	 * voltage/mode. Originally enabled consumers retain that enable request. */
	const unsigned off_order[] = { 1, 2, 0 }, on_order[] = { 0, 2, 1 };
	const unsigned *order = s->saved[rail][1] ? on_order : off_order;
	for (unsigned i = 0; i < 3; i++) {
		unsigned field = order[i];
		if (!(s->dirty[rail] & (1u << field))) continue;
		if (!raphael_power_put(s, ops, ctx, rail, field, s->saved[rail][field])) return false;
		s->dirty[rail] &= ~(1u << field);
	}
	return true;
}

static bool
raphael_power_end(struct raphael_power_state *s)
{
	if (!s || !s->active) return false;
	for (unsigned rail = 0; rail < 4; rail++) if (s->dirty[rail]) return false;
	s->active = false;
	return true;
}
