// SPDX-License-Identifier: GPL-2.0
/*
 * Symbolize an address inside a module function whose name is longer than
 * KSYM_NAME_LEN. The lookup must truncate the name to KSYM_NAME_LEN - 1
 * bytes and must not write outside the caller's KSYM_SYMBOL_LEN buffer.
 *
 * Only a module symtab can carry such a name (scripts/kallsyms drops it
 * from vmlinux), so this test is module-only.
 */

#include <kunit/test.h>
#include <linux/kallsyms.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/stringify.h>

#define DI(name) s##name##name
#define DDI(name) DI(n##name##name)
#define DDDI(name) DDI(n##name##name)
#define DDDDI(name) DDDI(n##name##name)
#define DDDDDI(name) DDDDI(n##name##name)
#define DDDDDDI(name) DDDDDI(n##name##name)

/* Generate a symbol whose name length is 1023 */
#define OVERLONG_SYM DDDDDDI(o1v2e3r4l5o6n7g)
#define OVERLONG_SYM_NAME __stringify(OVERLONG_SYM)

static_assert(sizeof(OVERLONG_SYM_NAME) == 2 * KSYM_NAME_LEN);

#define OVERLONG_SYM_RETURN 0x5a5a5
#define OVERLONG_SYM_GUARD 0xa5

/*
 * The 1023-byte name is the ELF symbol only (asm label): BTF validation
 * rejects names of KSYM_NAME_LEN bytes or more, so the C name, which is
 * what the module's BTF records, stays short.
 */
static noinline int overlong_sym(void) __asm__(OVERLONG_SYM_NAME);
static noinline int overlong_sym(void)
{
	return OVERLONG_SYM_RETURN;
}

struct overlong_sym_buf {
	char head[32];
	char sym[KSYM_SYMBOL_LEN];
	char tail[32];
};

static struct overlong_sym_buf *overlong_sym_buf(struct kunit *test)
{
	struct overlong_sym_buf *buf = kunit_kmalloc(test, sizeof(*buf), GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, OVERLONG_SYM_GUARD, sizeof(*buf));
	return buf;
}

static void overlong_sym_check(struct kunit *test, const struct overlong_sym_buf *buf,
			       int len, const char *expect)
{
	KUNIT_EXPECT_FALSE(test, memchr_inv(buf->head, OVERLONG_SYM_GUARD, sizeof(buf->head)));
	KUNIT_EXPECT_FALSE(test, memchr_inv(buf->tail, OVERLONG_SYM_GUARD, sizeof(buf->tail)));
	KUNIT_ASSERT_LT(test, strnlen(buf->sym, sizeof(buf->sym)), sizeof(buf->sym));
	KUNIT_EXPECT_EQ(test, len, (int)strlen(buf->sym));
	KUNIT_EXPECT_EQ(test, strncmp(buf->sym, expect, strlen(expect)), 0);
}

static void test_overlong_module_symbol_sprint(struct kunit *test)
{
	unsigned long addr = (unsigned long)overlong_sym;
	struct overlong_sym_buf *buf;
	char *expect;
	int len;

	KUNIT_ASSERT_EQ(test, overlong_sym(), OVERLONG_SYM_RETURN);
	expect = kunit_kmalloc(test, KSYM_SYMBOL_LEN, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, expect);

	buf = overlong_sym_buf(test);
	len = sprint_symbol_no_offset(buf->sym, addr);
	snprintf(expect, KSYM_SYMBOL_LEN, "%.*s [%s]", KSYM_NAME_LEN - 1,
		 OVERLONG_SYM_NAME, KBUILD_MODNAME);
	overlong_sym_check(test, buf, len, expect);
	KUNIT_EXPECT_STREQ(test, buf->sym, expect);

	buf = overlong_sym_buf(test);
	len = sprint_symbol(buf->sym, addr + 1);
	snprintf(expect, KSYM_SYMBOL_LEN, "%.*s+0x1/0x", KSYM_NAME_LEN - 1, OVERLONG_SYM_NAME);
	overlong_sym_check(test, buf, len, expect);
	KUNIT_EXPECT_NOT_NULL(test, strstr(buf->sym, " [" KBUILD_MODNAME "]"));
}

static struct kunit_case overlong_symbol_test_cases[] = {
	KUNIT_CASE(test_overlong_module_symbol_sprint),
	{}
};

static struct kunit_suite overlong_symbol_test_suite = {
	.name = "overlong-symbol",
	.test_cases = overlong_symbol_test_cases,
};
kunit_test_suite(overlong_symbol_test_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Test symbolizing a module symbol longer than KSYM_NAME_LEN");
