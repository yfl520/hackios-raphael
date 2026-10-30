/* cam=6: bounded sensor configuration, never stream-on or capture.
 * Validate the RAM firmware table before any board mutation. The existing
 * rear lifecycle asserts reset and restores supplies even on I2C failure.
 */
static bool
raphael_camera_configure_stopped(struct raphael_i2c_bus *bus, void *context)
{
	struct raphael_sensor *s = context;
	bool ok = raphael_sensor_prepare(s, bus);
	bool stopped = raphael_sensor_stop(s, bus);
	say("raphael-camera: mode-table prepare %s; stream-off %s\n",
	    ok ? "verified" : "FAILED", stopped ? "verified" : "FAILED");
	return ok && stopped;
}

static bool
raphael_camera_config_probe(void)
{
	struct raphael_sensor s = { 0 };
	uint64_t size = 0;
	const uint8_t *data = hakios_wifi_file("raphael-imx586.table", &size);
	if (!raphael_sensor_load(&s, data, size)) {
		say("raphael-camera: missing/invalid R586TB01; no board changes\n");
		return false;
	}
	say("raphael-camera: bounded mode-table probe; stream-on and DMA disabled\n");
	return raphael_camera_rear_probe(raphael_camera_configure_stopped, &s, NULL);
}
