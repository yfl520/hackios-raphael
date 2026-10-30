/* Read-only SM8150 SIM diagnostics on Wi-Fi's existing MPSS QRTR link.
 * Included by hakios.wifi.c after qrtr_ops. Every entry runs on link_thread,
 * including teardown: no second MPSS, callback owner, or transport thread.
 * Protocol: libqmi data/qmi-service-uim.json, UIM Card Status/Get Card Status;
 * data/qmi-service-dms.json, Get Operating Mode. Never log identifiers/AIDs.
 */
#define RAPHAEL_SIM_CARDS 8
#define RAPHAEL_SIM_APPS 16
struct raphael_sim_app { uint8_t type, state, pin1, retries; };
struct raphael_sim_card {
	uint8_t state, upin, error, apps;
	struct raphael_sim_app app[RAPHAEL_SIM_APPS];
};
struct raphael_sim_status {
	uint16_t primary;
	uint8_t cards;
	struct raphael_sim_card card[RAPHAEL_SIM_CARDS];
};

static uint16_t
raphael_sim_u16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

/* Commit only after the whole variable-length TLV has passed its bounds. */
static int
raphael_sim_decode(const uint8_t *v, size_t n, struct raphael_sim_status *out)
{
	struct raphael_sim_status s = { 0 };
	size_t at = 9;
	if (n < at || v[8] > RAPHAEL_SIM_CARDS) return EPROTO;
	s.primary = raphael_sim_u16(v);
	s.cards = v[8];
	for (unsigned c = 0; c < s.cards; c++) {
		if (n - at < 6) return EPROTO;
		struct raphael_sim_card *card = &s.card[c];
		card->state = v[at]; card->upin = v[at + 1]; card->error = v[at + 4]; card->apps = v[at + 5];
		if (card->apps > RAPHAEL_SIM_APPS) return EPROTO;
		at += 6;
		for (unsigned a = 0; a < card->apps; a++) {
			if (n - at < 7) return EPROTO;
			size_t aid = v[at + 6];
			if (n - at < 14 + aid) return EPROTO;
			struct raphael_sim_app *app = &card->app[a];
			app->type = v[at]; app->state = v[at + 1];
			app->pin1 = v[at + 8 + aid]; app->retries = v[at + 9 + aid];
			at += 14 + aid; /* skip AID without copying, printing, or retaining it */
		}
	}
	if (at != n) return EPROTO;
	*out = s;
	return 0;
}

struct raphael_sim_query {
	struct hakios_qrtr_port *local;
	uint32_t service, node, remote;
	uint16_t message, transaction;
	bool sent, done;
	int error;
};
static struct raphael_sim_query raphael_sim_queries[2];
static bool raphael_sim_started, raphael_sim_finished;
static uint64_t raphael_sim_deadline;

/* Only a matched response can complete a query. Validate every TLV before use. */
static void
raphael_sim_receive(void *context, uint32_t node, uint32_t remote, const void *data, size_t n)
{
	struct raphael_sim_query *q = context;
	const uint8_t *m = data, *result = NULL, *value = NULL;
	size_t value_n = 0, at = 7;
	if (!q->sent || q->done || node != q->node || remote != q->remote || n < 7 || m[0] != 2 ||
	    raphael_sim_u16(m + 1) != q->transaction || raphael_sim_u16(m + 3) != q->message ||
	    (size_t)raphael_sim_u16(m + 5) != n - 7) return;
	int error = 0;
	while (at < n) {
		if (n - at < 3) { error = EPROTO; break; }
		uint8_t type = m[at];
		size_t len = raphael_sim_u16(m + at + 1);
		at += 3;
		if (len > n - at) { error = EPROTO; break; }
		if (type == 2) {
			if (result || len != 4) { error = EPROTO; break; }
			result = m + at;
		}
		if (type == (q->service == 0x0b ? 0x10 : 0x01)) {
			if (value) { error = EPROTO; break; }
			value = m + at; value_n = len;
		}
		at += len;
	}
	if (!error && !result) error = EPROTO;
	if (!error && raphael_sim_u16(result)) {
		printf("raphael-sim: service=%#x message=%#x QMI error=%u\n", q->service, q->message,
		    raphael_sim_u16(result + 2));
		error = EIO;
	}
	if (!error && !value) error = EPROTO;
	if (!error && q->service == 0x0b) {
		struct raphael_sim_status status;
		error = raphael_sim_decode(value, value_n, &status);
		if (!error) {
			printf("raphael-sim: card-status OK cards=%u primary-GW=%#x\n", status.cards, status.primary);
			for (unsigned c = 0; c < status.cards; c++) {
				const struct raphael_sim_card *card = &status.card[c];
				printf("raphael-sim: slot=%u state=%u upin=%u error=%u apps=%u\n", c + 1,
				    card->state, card->upin, card->error, card->apps);
				for (unsigned a = 0; a < card->apps; a++) {
					const struct raphael_sim_app *app = &card->app[a];
					printf("raphael-sim: slot=%u app=%u type=%u state=%u pin1=%u retries=%u\n",
					    c + 1, a, app->type, app->state, app->pin1, app->retries);
				}
			}
		}
	} else if (!error) {
		/* Get Operating Mode has one mandatory byte, no device identity fields. */
		if (value_n != 1) error = EPROTO;
		else printf("raphael-sim: modem operating-mode=%u (0=online,1=low-power,2=factory,3=offline)\n", value[0]);
	}
	q->error = error; q->done = true;
	if (error) printf("raphael-sim: service=%#x response rejected errno=%d\n", q->service, error);
}

static void
raphael_sim_server(void *context, bool arrived, uint32_t service, uint32_t version, uint32_t instance,
    uint32_t node, uint32_t remote)
{
	struct raphael_sim_query *q = context;
	(void)version; (void)instance;
	if (q->done || q->service != service) return;
	if (!arrived) {
		if (q->sent && q->node == node && q->remote == remote) { q->done = true; q->error = ENXIO; }
		return;
	}
	if (q->sent) return;
	q->node = node; q->remote = remote; q->sent = true;
	/* QMI QRTR header: flag8, transaction16, message16, length16. No client-id. */
	uint8_t request[7] = { 0, q->transaction & 255, q->transaction >> 8,
	    q->message & 255, q->message >> 8, 0, 0 };
	q->error = qrtr_send(q->local, node, remote, request, sizeof(request));
	if (q->error) q->done = true;
	printf("raphael-sim: read-only request service=%#x message=%#x sent errno=%d\n", service, q->message, q->error);
}

static void
raphael_sim_close(void)
{
	if (!raphael_sim_started || raphael_sim_finished) return;
	for (unsigned i = 0; i < 2; i++) {
		struct raphael_sim_query *q = &raphael_sim_queries[i];
		if (q->local) { qrtr_close(q->local); q->local = NULL; }
	}
	raphael_sim_finished = true;
}

/* Called under qrtr_calling by the sole link thread, after dispatching packets. */
static void
raphael_sim_tick(void)
{
	if (raphael_sim_finished) return;
	if (!raphael_sim_started) {
		if (!glink.open) return;
		if (!hakios_is_raphael() || hakios_wifi_option("uim_probe") != 1) {
			raphael_sim_finished = true; return;
		}
		raphael_sim_started = true;
		raphael_sim_deadline = now_us() + 8000000ull;
		raphael_sim_queries[0].service = 0x0b; raphael_sim_queries[0].message = 0x2f;
		raphael_sim_queries[1].service = 0x02; raphael_sim_queries[1].message = 0x2d;
		for (unsigned i = 0; i < 2; i++) {
			struct raphael_sim_query *q = &raphael_sim_queries[i];
			q->transaction = (uint16_t)(0x7301 + i);
			q->local = qrtr_open(raphael_sim_receive, raphael_sim_server, q);
			q->error = q->local ? qrtr_lookup(q->local, q->service, 0, 0) : ENOSPC;
			if (q->error) q->done = true;
		}
		printf("raphael-sim: read-only probe armed on existing MPSS link, deadline=8s\n");
	}
	bool complete = raphael_sim_queries[0].done && raphael_sim_queries[1].done;
	if (complete || now_us() >= raphael_sim_deadline) {
		for (unsigned i = 0; i < 2; i++) {
			struct raphael_sim_query *q = &raphael_sim_queries[i];
			if (!q->done) { q->error = ETIMEDOUT; q->done = true; }
			printf("raphael-sim: probe completed service=%#x sent=%u errno=%d\n", q->service, q->sent, q->error);
		}
		raphael_sim_close();
	}
}
