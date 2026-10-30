/* Bounded RAM-session diagnostic: no persistent device settings or partition IO. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

static int
read_int(const char *name)
{
	int value = -1;
	size_t size = sizeof(value);
	if (sysctlbyname(name, &value, &size, NULL, 0))
		printf("peripheral_test: %s: %s\n", name, strerror(errno));
	else printf("peripheral_test: %s = %d\n", name, value);
	return value;
}

int
main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	read_int("kern.hakios.brightness");
	read_int("kern.hakios.brightness_backend");
	read_int("kern.hakios.brightness_status");
	int levels[] = {35, 65, 50};
	for (unsigned i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
		int value = levels[i];
		int error = sysctlbyname("kern.hakios.brightness", NULL, NULL, &value, sizeof(value));
		printf("peripheral_test: brightness request %d%%: %s\n", value, error ? strerror(errno) : "DMA completed");
		read_int("kern.hakios.brightness");
		read_int("kern.hakios.brightness_feedback");
		read_int("kern.hakios.brightness_status");
		if (error) break;
		sleep(3);
	}
	return 0;
}
