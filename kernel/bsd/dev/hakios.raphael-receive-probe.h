/* cam7: stopped-sensor capture-clock/PHY configuration and full restoration.
 * No stream-on or DMA. Retain the entire board session if a child cannot be
 * quiesced, rather than collapsing its parent domain underneath it.
 */
struct raphael_receive_probe {
	struct raphael_sensor sensor;
	bool retain_power, stream, mapping, frame;
};

static bool
raphael_receive_stopped(struct raphael_i2c_bus *bus, void *context)
{
	struct raphael_receive_probe *p = context;
	struct raphael_capture_clocks clocks = { 0 };
	struct raphael_phy phy = { 0 };
	struct raphael_csid_rx rx = { 0 };
	bool ok = raphael_sensor_prepare(&p->sensor, bus);
	if (ok) ok = raphael_capture_clocks_begin(&clocks);
	if (ok) {
		ok = raphael_phy_begin(&phy);
		say("raphael-camera: PHY0 v1.1 four-lane configure %s; sensor stopped\n",
		    ok ? "verified" : "FAILED");
	}
	if (ok && p->stream) {
		ok = raphael_csid_begin(&rx);
		if (ok) ok = raphael_csid_stream_probe(&rx, &p->sensor, bus);
	}
	if (ok && p->mapping) ok = raphael_camera_smmu_probe(&p->retain_power);
	if (ok && p->frame) ok = raphael_frame_capture(&p->sensor, bus, &p->retain_power);
	bool stopped = raphael_sensor_stop(&p->sensor, bus);
	bool rx_clean = !p->retain_power && stopped && raphael_csid_end(&rx);
	bool phy_clean = rx_clean && raphael_phy_end(&phy);
	bool clock_clean = phy_clean && raphael_capture_clocks_end(&clocks);
	p->retain_power = !phy_clean || !clock_clean;
	say("raphael-camera: receive probe %s; sensor-off=%u RX-restore=%u PHY-restore=%u clock-restore=%u\n",
	    ok && stopped && !p->retain_power ? "verified" : "FAILED",
	    stopped, rx_clean, phy_clean, clock_clean);
	return ok && stopped && !p->retain_power;
}

static bool
raphael_camera_receive_probe(bool stream, bool mapping, bool frame)
{
	struct raphael_receive_probe p = { .stream = stream, .mapping = mapping, .frame = frame };
	uint64_t size = 0;
	const uint8_t *data = hakios_wifi_file("raphael-imx586.table", &size);
	if (!raphael_sensor_load(&p.sensor, data, size)) {
		say("raphael-camera: invalid R586TB01; receive probe refused before power\n");
		return false;
	}
	return raphael_camera_rear_probe(raphael_receive_stopped, &p, &p.retain_power);
}
