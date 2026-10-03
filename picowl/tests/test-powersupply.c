/* test-powersupply.c - unit tests for power supply detection and uevent parsing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include "powersupply.h"

/* Stub implementation of pw_sysfs_root for testing */
const char *pw_sysfs_root(void)
{
	const char *r = getenv("PICOWL_SYSFS_ROOT");
	return (r && *r) ? r : "/sys";
}

static int fails;

#define EQ(got, want) do { int g_ = (got), w_ = (want); if (g_ != w_) { \
	fprintf(stderr, "%s:%d: got %d want %d\n", __FILE__, __LINE__, g_, w_); fails++; } } while (0)

#define PROFILE_EQ(got, want) do { enum pw_power_profile g_ = (got), w_ = (want); \
	if (g_ != w_) { \
		fprintf(stderr, "%s:%d: got profile %d want %d\n", __FILE__, __LINE__, g_, w_); fails++; } } while (0)

#define BOOL_EQ(got, want) do { bool g_ = (got), w_ = (want); if (g_ != w_) { \
	fprintf(stderr, "%s:%d: got %s want %s\n", __FILE__, __LINE__, g_ ? "true" : "false", w_ ? "true" : "false"); fails++; } } while (0)

/* Helper to set up a fake sysfs tree for testing */
static char test_root[256];

static void make_supply(const char *name, const char *type, int online, int capacity)
{
	char dir[384];
	snprintf(dir, sizeof(dir), "%s/class/power_supply/%s", test_root, name);
	mkdir(dir, 0755);

	/* Write type */
	char type_file[512];
	snprintf(type_file, sizeof(type_file), "%s/type", dir);
	FILE *f = fopen(type_file, "w");
	assert(f);
	fprintf(f, "%s\n", type);
	fclose(f);

	/* Write online if applicable */
	if (online >= 0) {
		char online_file[512];
		snprintf(online_file, sizeof(online_file), "%s/online", dir);
		f = fopen(online_file, "w");
		assert(f);
		fprintf(f, "%d\n", online);
		fclose(f);
	}

	/* Write capacity if applicable */
	if (capacity >= 0) {
		char cap_file[512];
		snprintf(cap_file, sizeof(cap_file), "%s/capacity", dir);
		f = fopen(cap_file, "w");
		assert(f);
		fprintf(f, "%d\n", capacity);
		fclose(f);
	}
}

static void setup_test_root(void)
{
	static int counter = 0;
	snprintf(test_root, sizeof(test_root), "/tmp/picowl-test-%d-%d", getpid(), counter++);

	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "mkdir -p %s/class/power_supply", test_root);
	assert(system(cmd) == 0);

	setenv("PICOWL_SYSFS_ROOT", test_root, 1);
}

static void cleanup_test_root(void)
{
	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "rm -rf %s", test_root);
	system(cmd);
	unsetenv("PICOWL_SYSFS_ROOT");
}

/* Tests for pw_ps_read_profile */

static int test_ac_online(void)
{
	setup_test_root();

	make_supply("AC", "Mains", 1, -1);
	make_supply("BAT", "Battery", -1, 80);

	struct pw_ps_info info;
	enum pw_power_profile profile = pw_ps_read_profile(test_root, 15, &info);

	PROFILE_EQ(profile, PW_PROFILE_AC);
	BOOL_EQ(info.any_supply, true);
	BOOL_EQ(info.ac_online, true);
	EQ(info.capacity, 80);

	cleanup_test_root();
	printf("✓ test_ac_online\n");
	return 0;
}

static int test_usb_online(void)
{
	setup_test_root();

	make_supply("USB", "USB", 1, -1);
	make_supply("BAT", "Battery", -1, 50);

	struct pw_ps_info info;
	enum pw_power_profile profile = pw_ps_read_profile(test_root, 15, &info);

	PROFILE_EQ(profile, PW_PROFILE_AC);
	BOOL_EQ(info.ac_online, true);

	cleanup_test_root();
	printf("✓ test_usb_online\n");
	return 0;
}

static int test_battery_high(void)
{
	setup_test_root();

	make_supply("BAT", "Battery", -1, 80);

	struct pw_ps_info info;
	enum pw_power_profile profile = pw_ps_read_profile(test_root, 15, &info);

	PROFILE_EQ(profile, PW_PROFILE_BATTERY);
	BOOL_EQ(info.ac_online, false);
	EQ(info.capacity, 80);

	cleanup_test_root();
	printf("✓ test_battery_high\n");
	return 0;
}

static int test_battery_low(void)
{
	setup_test_root();

	make_supply("BAT", "Battery", -1, 10);

	struct pw_ps_info info;
	enum pw_power_profile profile = pw_ps_read_profile(test_root, 15, &info);

	PROFILE_EQ(profile, PW_PROFILE_LOW);
	EQ(info.capacity, 10);

	cleanup_test_root();
	printf("✓ test_battery_low\n");
	return 0;
}

static int test_battery_at_threshold(void)
{
	setup_test_root();

	/* Exactly at the low_capacity threshold */
	make_supply("BAT", "Battery", -1, 15);

	struct pw_ps_info info;
	enum pw_power_profile profile = pw_ps_read_profile(test_root, 15, &info);

	PROFILE_EQ(profile, PW_PROFILE_LOW);
	EQ(info.capacity, 15);

	cleanup_test_root();
	printf("✓ test_battery_at_threshold\n");
	return 0;
}

static int test_multiple_batteries_lowest(void)
{
	setup_test_root();

	make_supply("BAT0", "Battery", -1, 60);
	make_supply("BAT1", "Battery", -1, 10);
	make_supply("BAT2", "Battery", -1, 40);

	struct pw_ps_info info;
	enum pw_power_profile profile = pw_ps_read_profile(test_root, 15, &info);

	PROFILE_EQ(profile, PW_PROFILE_LOW);
	EQ(info.capacity, 10);  /* Should be the lowest */

	cleanup_test_root();
	printf("✓ test_multiple_batteries_lowest\n");
	return 0;
}

static int test_no_supplies(void)
{
	setup_test_root();
	/* Empty power_supply directory */

	struct pw_ps_info info;
	enum pw_power_profile profile = pw_ps_read_profile(test_root, 15, &info);

	PROFILE_EQ(profile, PW_PROFILE_BATTERY);
	BOOL_EQ(info.any_supply, false);
	BOOL_EQ(info.ac_online, false);
	EQ(info.capacity, -1);

	cleanup_test_root();
	printf("✓ test_no_supplies\n");
	return 0;
}

static int test_unreadable_capacity(void)
{
	setup_test_root();

	/* Create a Battery without a capacity file */
	char dir[384];
	snprintf(dir, sizeof(dir), "%s/class/power_supply/BAT", test_root);
	mkdir(dir, 0755);

	char type_file[512];
	snprintf(type_file, sizeof(type_file), "%s/type", dir);
	FILE *f = fopen(type_file, "w");
	fprintf(f, "Battery\n");
	fclose(f);

	struct pw_ps_info info;
	enum pw_power_profile profile = pw_ps_read_profile(test_root, 15, &info);

	PROFILE_EQ(profile, PW_PROFILE_BATTERY);
	BOOL_EQ(info.any_supply, true);

	cleanup_test_root();
	printf("✓ test_unreadable_capacity\n");
	return 0;
}

/* Tests for pw_ps_uevent_is_power_supply */

static int test_uevent_parser_power_supply(void)
{
	/* Valid power_supply uevent message */
	const char msg[] = "add@/devices/virtual/power_supply/AC\0"
	                   "ACTION=add\0"
	                   "DEVPATH=/devices/virtual/power_supply/AC\0"
	                   "SUBSYSTEM=power_supply\0";

	BOOL_EQ(pw_ps_uevent_is_power_supply(msg, sizeof(msg) - 1), true);
	printf("✓ test_uevent_parser_power_supply\n");
	return 0;
}

static int test_uevent_parser_other_subsystem(void)
{
	/* Uevent with a different subsystem */
	const char msg[] = "add@/sys/devices/pci0000:00/0000:00:14.0/usb1\0"
	                   "ACTION=add\0"
	                   "DEVPATH=/devices/pci0000:00/0000:00:14.0/usb1\0"
	                   "SUBSYSTEM=usb\0";

	BOOL_EQ(pw_ps_uevent_is_power_supply(msg, sizeof(msg) - 1), false);
	printf("✓ test_uevent_parser_other_subsystem\n");
	return 0;
}

static int test_uevent_parser_no_subsystem(void)
{
	/* Uevent without SUBSYSTEM field */
	const char msg[] = "add@/devices/virtual/power_supply/AC\0"
	                   "ACTION=add\0"
	                   "DEVPATH=/devices/virtual/power_supply/AC\0";

	BOOL_EQ(pw_ps_uevent_is_power_supply(msg, sizeof(msg) - 1), false);
	printf("✓ test_uevent_parser_no_subsystem\n");
	return 0;
}

static int test_uevent_parser_truncated(void)
{
	/* Truncated message (no NUL terminator for the subsystem value) */
	const char msg[] = "add@/devices/virtual/power_supply/AC\0"
	                   "ACTION=add\0"
	                   "SUBSYSTEM=power";

	BOOL_EQ(pw_ps_uevent_is_power_supply(msg, sizeof(msg) - 1), false);
	printf("✓ test_uevent_parser_truncated\n");
	return 0;
}

static int test_uevent_parser_empty_buffer(void)
{
	const char msg[] = "";
	BOOL_EQ(pw_ps_uevent_is_power_supply(msg, 0), false);
	printf("✓ test_uevent_parser_empty_buffer\n");
	return 0;
}

static int test_uevent_parser_single_nul(void)
{
	const char msg[] = "\0";
	BOOL_EQ(pw_ps_uevent_is_power_supply(msg, 1), false);
	printf("✓ test_uevent_parser_single_nul\n");
	return 0;
}

static int test_uevent_parser_multiple_fields(void)
{
	/* Message with many fields, subsystem in the middle */
	const char msg[] = "change@/devices/virtual/power_supply/BAT\0"
	                   "ACTION=change\0"
	                   "DEVPATH=/devices/virtual/power_supply/BAT\0"
	                   "SUBSYSTEM=power_supply\0"
	                   "POWER_SUPPLY_NAME=BAT\0"
	                   "POWER_SUPPLY_STATUS=Discharging\0";

	BOOL_EQ(pw_ps_uevent_is_power_supply(msg, sizeof(msg) - 1), true);
	printf("✓ test_uevent_parser_multiple_fields\n");
	return 0;
}

static int test_uevent_parser_partial_subsystem(void)
{
	/* Message where a field starts with "SUBSYSTEM=" but has different value */
	const char msg[] = "change@/devices/virtual/power_supply/BAT\0"
	                   "ACTION=change\0"
	                   "SUBSYSTEM=platform\0";

	BOOL_EQ(pw_ps_uevent_is_power_supply(msg, sizeof(msg) - 1), false);
	printf("✓ test_uevent_parser_partial_subsystem\n");
	return 0;
}

int main(void)
{
	/* Test pw_ps_read_profile */
	test_ac_online();
	test_usb_online();
	test_battery_high();
	test_battery_low();
	test_battery_at_threshold();
	test_multiple_batteries_lowest();
	test_no_supplies();
	test_unreadable_capacity();

	/* Test pw_ps_uevent_is_power_supply */
	test_uevent_parser_power_supply();
	test_uevent_parser_other_subsystem();
	test_uevent_parser_no_subsystem();
	test_uevent_parser_truncated();
	test_uevent_parser_empty_buffer();
	test_uevent_parser_single_nul();
	test_uevent_parser_multiple_fields();
	test_uevent_parser_partial_subsystem();

	if (fails > 0) {
		fprintf(stderr, "FAIL: %d test(s) failed\n", fails);
		return 1;
	}

	printf("All tests passed!\n");
	return 0;
}
