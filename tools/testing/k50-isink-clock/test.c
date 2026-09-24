#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mt6353_hw.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define pr_err(...) fprintf(stderr, __VA_ARGS__)
#define CHECK(condition, message) do { \
	if (!(condition)) { \
		fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); \
		exit(1); \
	} \
} while (0)

#include "pmic_fields.inc"

static uint16_t registers[0x400];
struct field_write {
	PMU_FLAGS_LIST_ENUM flag;
	unsigned int value;
};
static struct field_write trace[128];
static unsigned int trace_count;

static const PMU_FLAG_TABLE_ENTRY *field(PMU_FLAGS_LIST_ENUM flag)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(fields); i++)
		if (fields[i].flagname == flag)
			return &fields[i];
	CHECK(0, "unknown PMIC field");
	return NULL;
}

static unsigned int pmic_config_interface(unsigned int addr, unsigned int value,
					  unsigned int mask, unsigned int shift)
{
	CHECK(!(addr & 1) && addr / 2 < ARRAY_SIZE(registers), "register address");
	CHECK(value <= mask && (mask << shift) <= 0xffff, "field bounds");
	registers[addr / 2] &= ~(mask << shift);
	registers[addr / 2] |= value << shift;
	return 0;
}

static unsigned short pmic_set_register_value(PMU_FLAGS_LIST_ENUM flag,
					       unsigned int value)
{
	const PMU_FLAG_TABLE_ENTRY *f = field(flag);

	CHECK(trace_count < ARRAY_SIZE(trace), "trace capacity");
	trace[trace_count++] = (struct field_write){flag, value};
	return pmic_config_interface(f->offset, value, f->mask, f->shift);
}

static unsigned int read_field(PMU_FLAGS_LIST_ENUM flag)
{
	const PMU_FLAG_TABLE_ENTRY *f = field(flag);

	return (registers[f->offset / 2] >> f->shift) & f->mask;
}

#include "production.inc"

/* Existing private cleanup order and values must remain unchanged. */
static const struct field_write private_cleanup[] = {
	{PMIC_ISINK_CH3_EN, 0}, {PMIC_ISINK_CH2_EN, 0},
	{PMIC_ISINK_CHOP3_EN, 0}, {PMIC_ISINK_CHOP2_EN, 0},
	{PMIC_ISINK_CH3_BIAS_EN, 0}, {PMIC_ISINK_CH2_BIAS_EN, 0},
	{PMIC_RG_ISINK2_DOUBLE_EN, 0}, {PMIC_RG_ISINK3_DOUBLE_EN, 0},
	{PMIC_CLK_DRV_ISINK3_CK_PDN, 1}, {PMIC_CLK_DRV_ISINK2_CK_PDN, 1},
	{PMIC_CLK_DRV_CHRIND_CK_PDN, 1},
};
static const PMU_FLAGS_LIST_ENUM torch_fields[] = {
	PMIC_ISINK_CH0_EN, PMIC_ISINK_CH1_EN,
	PMIC_CLK_DRV_ISINK0_CK_PDN, PMIC_CLK_DRV_ISINK1_CK_PDN,
	PMIC_ISINK_CH0_MODE, PMIC_ISINK_CH1_MODE,
	PMIC_ISINK_CH0_STEP, PMIC_ISINK_CH1_STEP,
	PMIC_ISINK_DIM0_DUTY, PMIC_ISINK_DIM1_DUTY,
	PMIC_ISINK_DIM0_FSEL, PMIC_ISINK_DIM1_FSEL,
};

static void reset(void)
{
	memset(registers, 0, sizeof(registers));
	registers[MT6353_CLK_CKPDN_CON0 / 2] = 0xffff;
	registers[MT6353_CLK_CKPDN_CON1 / 2] = 0xffff;
	trace_count = 0;
}

static void cleanup_dummy(void)
{
	unsigned int i, next = 0;

	trace_count = 0;
	enable_dummy_load(0);
	for (i = 0; i < trace_count; i++) {
		/* The old shared-clock write is checked by its effect below. */
		if (trace[i].flag == PMIC_CLK_DRV_32K_CK_PDN)
			continue;
		CHECK(next < ARRAY_SIZE(private_cleanup), "extra private cleanup write");
		CHECK(trace[i].flag == private_cleanup[next].flag &&
		      trace[i].value == private_cleanup[next].value,
		      "private cleanup order or value changed");
		next++;
	}
	CHECK(next == ARRAY_SIZE(private_cleanup), "missing private cleanup write");
	for (i = 0; i < ARRAY_SIZE(private_cleanup); i++)
		CHECK(read_field(private_cleanup[i].flag) == private_cleanup[i].value,
		      "private cleanup register value");
}

static void active_torch(int dummy_first)
{
	unsigned int before[ARRAY_SIZE(torch_fields)];
	unsigned int i, cycle;

	reset();
	if (dummy_first)
		enable_dummy_load(1);
	CHECK(flash_init() == 0 && flash_enable() == 0, "production torch setup");
	for (i = 0; i < ARRAY_SIZE(torch_fields); i++)
		before[i] = read_field(torch_fields[i]);
	for (cycle = 0; cycle < 3; cycle++) {
		if (!dummy_first || cycle)
			enable_dummy_load(1);
		CHECK(read_field(PMIC_ISINK_CH2_EN) && read_field(PMIC_ISINK_CH3_EN),
		      "dummy channels enabled");
		cleanup_dummy();
		CHECK(read_field(PMIC_CLK_DRV_32K_CK_PDN) == 0,
		      "DLPT cleanup gated the active torch clock");
		for (i = 0; i < ARRAY_SIZE(torch_fields); i++)
			CHECK(read_field(torch_fields[i]) == before[i], "torch configuration changed");
		CHECK(read_field(PMIC_ISINK_CH0_EN) && read_field(PMIC_ISINK_CH1_EN),
		      "torch channels remain enabled");
	}
}

int main(void)
{
	active_torch(0);
	active_torch(1);
	reset();
	enable_dummy_load(1);
	cleanup_dummy();
	CHECK(read_field(PMIC_CLK_DRV_32K_CK_PDN) == 0,
	      "dummy cleanup must follow the shared-clock ungate-only convention");
	CHECK(!read_field(PMIC_ISINK_CH0_EN) && !read_field(PMIC_ISINK_CH1_EN),
	      "idle dummy cycle must not enable torch");
	reset();
	cleanup_dummy();
	CHECK(read_field(PMIC_CLK_DRV_32K_CK_PDN) == 1,
	      "cleanup alone changed an initially gated shared clock");
	pmic_set_register_value(PMIC_CLK_DRV_32K_CK_PDN, 0);
	cleanup_dummy();
	CHECK(read_field(PMIC_CLK_DRV_32K_CK_PDN) == 0,
	      "cleanup alone changed an initially ungated shared clock");
	puts("PASS: 5 ISINK shared-clock scenarios; private cleanup unchanged");
	return 0;
}
