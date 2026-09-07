#include <stdio.h>
#include <stdlib.h>

#define K50_HALL_HOST_TEST
#include "../../../drivers/misc/mediatek/gpiokey/k50_gpiokey.h"

#define EXPECT_EQ(expected, actual) do { \
	int expected_value = (expected); \
	int actual_value = (actual); \
	if (expected_value != actual_value) { \
		fprintf(stderr, "%s:%d: expected %d, got %d\n", __FILE__, \
			__LINE__, expected_value, actual_value); \
		exit(EXIT_FAILURE); \
	} \
} while (0)

static void test_transition(int current, int sample, bool changed,
			    u32 next_irq_type)
{
	struct k50_hall_transition transition;

	EXPECT_EQ(0, k50_hall_transition(current, sample, &transition));
	EXPECT_EQ(changed, transition.changed);
	EXPECT_EQ(sample, transition.state);
	EXPECT_EQ(next_irq_type, transition.next_irq_type);
}

int main(void)
{
	struct k50_hall_transition transition;

	test_transition(0, 0, false, IRQ_TYPE_LEVEL_HIGH);
	test_transition(0, 1, true, IRQ_TYPE_LEVEL_LOW);
	test_transition(1, 1, false, IRQ_TYPE_LEVEL_LOW);
	test_transition(1, 0, true, IRQ_TYPE_LEVEL_HIGH);

	EXPECT_EQ(-EINVAL, k50_hall_transition(-1, 0, &transition));
	EXPECT_EQ(-EINVAL, k50_hall_transition(0, 2, &transition));
	EXPECT_EQ(-EIO, k50_hall_transition(0, -EIO, &transition));
	EXPECT_EQ(-EINVAL, k50_hall_transition(0, 1, NULL));

	puts("GPIOKEY HOST CONTRACT: PASS");
	return EXIT_SUCCESS;
}
