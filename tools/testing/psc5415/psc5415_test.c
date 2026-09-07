#define PSC5415_HOST_TEST
#include "../../../drivers/misc/mediatek/power/mt6755/psc5415.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum event_type {
	EVENT_READ,
	EVENT_WRITE,
};

struct event {
	enum event_type type;
	u8 reg;
	u8 value;
};

struct fake_bus {
	u8 registers[PSC5415_REG_COUNT];
	struct event events[64];
	unsigned int event_count;
	unsigned int read_calls;
	unsigned int write_calls;
	unsigned int fail_read_call;
	unsigned int fail_write_call;
};

static void fail(const char *message)
{
	fprintf(stderr, "PSC5415 TEST FAILURE: %s\n", message);
	exit(1);
}

static void require(int condition, const char *message)
{
	if (!condition)
		fail(message);
}

static void record_event(struct fake_bus *bus, enum event_type type,
	u8 reg, u8 value)
{
	if (bus->event_count >= sizeof(bus->events) / sizeof(bus->events[0]))
		fail("event buffer overflow");
	bus->events[bus->event_count].type = type;
	bus->events[bus->event_count].reg = reg;
	bus->events[bus->event_count].value = value;
	bus->event_count++;
}

static int fake_read(void *context, u8 reg, u8 *value)
{
	struct fake_bus *bus = context;

	bus->read_calls++;
	record_event(bus, EVENT_READ, reg, bus->registers[reg]);
	if (bus->fail_read_call == bus->read_calls)
		return -EIO;
	*value = bus->registers[reg];
	return 0;
}

static int fake_write(void *context, u8 reg, u8 value)
{
	struct fake_bus *bus = context;

	bus->write_calls++;
	record_event(bus, EVENT_WRITE, reg, value);
	if (bus->fail_write_call == bus->write_calls)
		return -EIO;
	bus->registers[reg] = value;
	return 0;
}

static struct psc5415_io fake_io(struct fake_bus *bus)
{
	struct psc5415_io io = {
		.context = bus,
		.read = fake_read,
		.write = fake_write,
	};

	return io;
}

static void require_event(const struct fake_bus *bus, unsigned int index,
	enum event_type type, u8 reg, u8 value, const char *message)
{
	require(index < bus->event_count, message);
	require(bus->events[index].type == type, message);
	require(bus->events[index].reg == reg, message);
	if (type == EVENT_WRITE)
		require(bus->events[index].value == value, message);
}

static void test_probe_sequence(void)
{
	struct fake_bus bus = { .registers = { 0 } };
	struct psc5415_io io = fake_io(&bus);
	u8 id = 0;

	bus.registers[PSC5415_REG_IC_INFO] = PSC5415_EXPECTED_IC_INFO;
	require(psc5415_apply_probe_sequence(&io, &id) == 0,
		"probe sequence rejected the expected IC");
	require(id == PSC5415_EXPECTED_IC_INFO, "probe returned the wrong IC ID");
	require(bus.event_count == 6, "probe emitted the wrong event count");
	require_event(&bus, 0, EVENT_READ, PSC5415_REG_IC_INFO, 0,
		"probe did not read IC_INFO first");
	require_event(&bus, 1, EVENT_WRITE, PSC5415_REG_SAFETY, 0x5f,
		"probe did not write safety first");
	require_event(&bus, 2, EVENT_WRITE, PSC5415_REG_IBAT, 0x50,
		"probe IBAT differs from stock");
	require_event(&bus, 3, EVENT_WRITE, PSC5415_REG_CONTROL, 0xfc,
		"probe enabled charging before policy initialization");
	require_event(&bus, 4, EVENT_WRITE, PSC5415_REG_OREG, 0xb6,
		"probe OREG differs from stock");
	require_event(&bus, 5, EVENT_WRITE, PSC5415_REG_SP_CHARGER, 0x84,
		"probe SP differs from stock");
}

static void test_transfer_status(void)
{
	require(psc5415_transfer_status(0x101, 0x101) == 0,
		"combined read rejected its exact transfer length");
	require(psc5415_transfer_status(1, 0x101) == -EIO,
		"combined read accepted a short transfer");
	require(psc5415_transfer_status(0, 2) == -EIO,
		"write accepted a zero-length transfer");
	require(psc5415_transfer_status(-ENODEV, 2) == -ENODEV,
		"transport did not preserve a negative bus error");
}

static void test_probe_failures(void)
{
	struct fake_bus bus;
	struct psc5415_io io;
	unsigned int failing_write;

	require(psc5415_apply_probe_sequence(NULL, NULL) == -EINVAL,
		"probe accepted a missing transport");

	memset(&bus, 0, sizeof(bus));
	bus.fail_read_call = 1;
	io = fake_io(&bus);
	require(psc5415_apply_probe_sequence(&io, NULL) == -EIO,
		"probe accepted an IC_INFO read failure");
	require(bus.write_calls == 0, "probe wrote after IC_INFO read failure");

	memset(&bus, 0, sizeof(bus));
	bus.registers[PSC5415_REG_IC_INFO] = 0x70;
	io = fake_io(&bus);
	require(psc5415_apply_probe_sequence(&io, NULL) == -ENODEV,
		"probe accepted the wrong IC identity");
	require(bus.write_calls == 0, "probe wrote to the wrong IC");

	for (failing_write = 1; failing_write <= 5; failing_write++) {
		memset(&bus, 0, sizeof(bus));
		bus.registers[PSC5415_REG_IC_INFO] = PSC5415_EXPECTED_IC_INFO;
		bus.fail_write_call = failing_write;
		io = fake_io(&bus);
		require(psc5415_apply_probe_sequence(&io, NULL) == -EIO,
			"probe ignored an injected write failure");
		require(bus.write_calls == failing_write,
			"probe wrote registers after an injected failure");
	}
}

static void test_operational_sequence(void)
{
	struct fake_bus bus = { .registers = { 0 } };
	struct psc5415_io io = fake_io(&bus);
	unsigned int failing_write;

	require(psc5415_apply_operational_sequence(NULL) == -EINVAL,
		"operational sequence accepted a missing transport");

	require(psc5415_apply_operational_sequence(&io) == 0,
		"operational sequence failed");
	require(bus.event_count == 4, "operational event count differs from stock");
	require_event(&bus, 0, EVENT_WRITE, PSC5415_REG_OREG, 0x2c,
		"operational OREG differs from stock");
	require_event(&bus, 1, EVENT_WRITE, PSC5415_REG_CONTROL, 0xfc,
		"operational initialization enabled charging before limits");
	require_event(&bus, 2, EVENT_WRITE, PSC5415_REG_SP_CHARGER, 0x02,
		"operational SP differs from stock");
	require_event(&bus, 3, EVENT_WRITE, PSC5415_REG_IBAT, 0x50,
		"operational IBAT differs from stock");

	for (failing_write = 1; failing_write <= 4; failing_write++) {
		memset(&bus, 0, sizeof(bus));
		bus.fail_write_call = failing_write;
		io = fake_io(&bus);
		require(psc5415_apply_operational_sequence(&io) == -EIO,
			"operational sequence ignored a write failure");
		require(bus.write_calls == failing_write,
			"operational sequence wrote after a failed register");
	}
}

static void test_rmw_fail_closed(void)
{
	struct fake_bus bus = { .registers = { 0 } };
	struct psc5415_io io = fake_io(&bus);

	bus.fail_read_call = 1;
	require(psc5415_update_bits(&io, PSC5415_REG_CONTROL, 1,
		PSC5415_CE_MASK, PSC5415_CE_SHIFT) == -EIO,
		"RMW accepted a read failure");
	require(bus.write_calls == 0, "RMW wrote after a read failure");

	memset(&bus, 0, sizeof(bus));
	bus.fail_write_call = 1;
	io = fake_io(&bus);
	require(psc5415_update_bits(&io, PSC5415_REG_CONTROL, 1,
		PSC5415_CE_MASK, PSC5415_CE_SHIFT) == -EIO,
		"RMW ignored a write failure");
	require(bus.write_calls == 1, "RMW retried a failed write");

	memset(&bus, 0, sizeof(bus));
	bus.registers[PSC5415_REG_CONTROL] = 0x38;
	io = fake_io(&bus);
	require(psc5415_update_bits(&io, PSC5415_REG_CONTROL, 7,
		PSC5415_IINLIM_MASK, PSC5415_IINLIM_SHIFT) == 0,
		"RMW failed to mask stock's value seven");
	require(bus.registers[PSC5415_REG_CONTROL] == 0xf8,
		"RMW did not mask value seven to IINLIM code three");

	memset(&bus, 0, sizeof(bus));
	bus.registers[PSC5415_REG_IBAT] = 0xff;
	io = fake_io(&bus);
	require(psc5415_update_bits(&io, PSC5415_REG_IBAT, 0,
		PSC5415_IOCHARGE_MASK, PSC5415_IOCHARGE_SHIFT) == 0,
		"REG4 RMW failed");
	require((bus.registers[PSC5415_REG_IBAT] & 0x80) == 0,
		"REG4 RMW replayed the self-clearing reset bit");

	require(psc5415_update_bits(NULL, PSC5415_REG_CONTROL, 0,
		PSC5415_CE_MASK, PSC5415_CE_SHIFT) == -EINVAL,
		"RMW accepted a missing transport");
	require(psc5415_update_bits(&io, PSC5415_REG_COUNT, 0,
		PSC5415_CE_MASK, PSC5415_CE_SHIFT) == -EINVAL,
		"RMW accepted an out-of-range register");
}

static void test_cv_policy(void)
{
	u8 code = 0xff;

	require(psc5415_cv_code(4200000, &code) == 0 && code == 35,
		"4.20 V boundary selected the wrong group");
	require(psc5415_cv_code(4200001, &code) == 0 && code == 35,
		"4.20 V request rounded up to 4.35 V");
	require(psc5415_cv_code(4349999, &code) == 0 && code == 35,
		"CV request below 4.35 V rounded up");
	require(psc5415_cv_code(4350000, &code) == 0 && code == 44,
		"4.35 V boundary selected the wrong group");
	require(psc5415_cv_code(4400000, &code) == 0 && code == 44,
		"normal charge request exceeded the board's 4.35 V cap");
	require(psc5415_cv_code(4500000, &code) == 0 && code == 44,
		"high CV request exceeded the board's 4.35 V cap");
	code = 0xa5;
	require(psc5415_cv_code(4199999, &code) == -ERANGE,
		"unsupported low CV request was not rejected");
	require(code == 0xa5,
		"rejected CV request still changed the selected register code");
	require(psc5415_cv_code(3500000, &code) == -ERANGE,
		"obsolete linear voltage mapping was accepted");
	require(psc5415_cv_code(4200000, NULL) == -EINVAL,
		"CV policy accepted a missing output pointer");
}

static void test_otg_policy(void)
{
	struct fake_bus bus = { .registers = { 0 } };
	struct psc5415_io io = fake_io(&bus);

	require(psc5415_apply_otg(NULL, true) == -EINVAL,
		"OTG policy accepted a missing transport");

	bus.registers[PSC5415_REG_CONTROL] = 0x7a;
	require(psc5415_apply_otg(&io, true) == 0, "OTG enable failed");
	require(bus.event_count == 4, "OTG enable emitted the wrong event count");
	require_event(&bus, 0, EVENT_READ, PSC5415_REG_CONTROL, 0,
		"OTG enable did not read before clearing HZ");
	require_event(&bus, 1, EVENT_WRITE, PSC5415_REG_CONTROL, 0x78,
		"OTG enable did not clear HZ first");
	require_event(&bus, 2, EVENT_READ, PSC5415_REG_CONTROL, 0,
		"OTG enable did not read before setting OPA");
	require_event(&bus, 3, EVENT_WRITE, PSC5415_REG_CONTROL, 0x79,
		"OTG enable did not set OPA second");

	memset(&bus, 0, sizeof(bus));
	bus.registers[PSC5415_REG_CONTROL] = 0x79;
	io = fake_io(&bus);
	require(psc5415_apply_otg(&io, false) == 0, "OTG disable failed");
	require(bus.event_count == 2, "OTG disable emitted the wrong event count");
	require_event(&bus, 1, EVENT_WRITE, PSC5415_REG_CONTROL, 0x78,
		"OTG disable did not clear OPA");

	memset(&bus, 0, sizeof(bus));
	bus.registers[PSC5415_REG_CONTROL] = 0x7a;
	bus.fail_write_call = 1;
	io = fake_io(&bus);
	require(psc5415_apply_otg(&io, true) == -EIO,
		"OTG enable ignored its HZ write failure");
	require(bus.write_calls == 1,
		"OTG enable set OPA after its HZ write failed");
}

static void test_current_policy(void)
{
	u8 code = 0xff;
	u32 rejected[] = { 0, 1, 35000, 49999, 50001, 65000, 80000, 204999 };
	unsigned int i;

	require(psc5415_charge_current_code(50000, &code) == 0 && code == 0,
		"USB current did not retain the stock minimum preset");
	require(psc5415_charge_current_code(205000, &code) == 0 && code == 7,
		"normal DCP current did not retain the stock high preset");
	for (i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
		code = 0xa5;
		require(psc5415_charge_current_code(rejected[i], &code) == -ERANGE,
			"unverified battery-current limit was accepted");
		require(code == 0xa5, "rejected current changed the output code");
	}
	require(psc5415_charge_current_code(50000, NULL) == -EINVAL,
		"charge-current mapping accepted a missing output pointer");

	code = 0xa5;
	require(psc5415_input_current_code(14999, &code) == -ERANGE && code == 0xa5,
		"input mapping accepted less than the variant-safe minimum");
	require(psc5415_input_current_code(15000, &code) == 0 && code == 0,
		"150 mA input did not use the lowest finite limit");
	require(psc5415_input_current_code(49999, &code) == 0 && code == 0,
		"input below 500 mA was rounded up");
	require(psc5415_input_current_code(50000, &code) == 0 && code == 1,
		"500 mA input mapping differs from the documented limit");
	require(psc5415_input_current_code(79999, &code) == 0 && code == 1,
		"input below 800 mA was rounded up");
	require(psc5415_input_current_code(80000, &code) == 0 && code == 2,
		"800 mA input mapping differs from the documented limit");
	require(psc5415_input_current_code(320000, &code) == 0 && code == 2,
		"finite DCP input request selected no limit");
	require(psc5415_input_current_code(50000, NULL) == -EINVAL,
		"input mapping accepted a missing output pointer");
}

int main(void)
{
	test_transfer_status();
	test_probe_sequence();
	test_probe_failures();
	test_operational_sequence();
	test_rmw_fail_closed();
	test_cv_policy();
	test_otg_policy();
	test_current_policy();
	puts("PSC5415 FAKE-I2C MATRIX: PASS");
	return 0;
}
