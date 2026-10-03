/* test-copytype.c - unit tests for copy-type driver detection. */
#include <assert.h>
#include <stdio.h>
#include "../src/copytype.h"

int main(void)
{
	enum pw_copy_override ov;

	/* Test pw_copytype_parse - auto/yes/no and variants */
	assert(pw_copytype_parse("auto", &ov) && ov == PW_COPY_AUTO);
	assert(pw_copytype_parse("Auto", &ov) && ov == PW_COPY_AUTO);
	assert(pw_copytype_parse("AUTO", &ov) && ov == PW_COPY_AUTO);

	assert(pw_copytype_parse("yes", &ov) && ov == PW_COPY_YES);
	assert(pw_copytype_parse("YES", &ov) && ov == PW_COPY_YES);
	assert(pw_copytype_parse("true", &ov) && ov == PW_COPY_YES);
	assert(pw_copytype_parse("TRUE", &ov) && ov == PW_COPY_YES);
	assert(pw_copytype_parse("1", &ov) && ov == PW_COPY_YES);
	assert(pw_copytype_parse("on", &ov) && ov == PW_COPY_YES);
	assert(pw_copytype_parse("ON", &ov) && ov == PW_COPY_YES);

	assert(pw_copytype_parse("no", &ov) && ov == PW_COPY_NO);
	assert(pw_copytype_parse("NO", &ov) && ov == PW_COPY_NO);
	assert(pw_copytype_parse("false", &ov) && ov == PW_COPY_NO);
	assert(pw_copytype_parse("FALSE", &ov) && ov == PW_COPY_NO);
	assert(pw_copytype_parse("0", &ov) && ov == PW_COPY_NO);
	assert(pw_copytype_parse("off", &ov) && ov == PW_COPY_NO);
	assert(pw_copytype_parse("OFF", &ov) && ov == PW_COPY_NO);

	assert(!pw_copytype_parse("invalid", &ov));
	assert(!pw_copytype_parse("", &ov));
	assert(!pw_copytype_parse(NULL, &ov));
	assert(!pw_copytype_parse("auto", NULL));
	printf("✓ pw_copytype_parse\n");

	/* Test pw_copytype_driver - copy-type table */
	/* Copy-type drivers */
	assert(pw_copytype_driver("mq11xx"));
	assert(pw_copytype_driver("MQ11XX"));
	assert(pw_copytype_driver("mediaq"));
	assert(pw_copytype_driver("MediaQ"));
	assert(pw_copytype_driver("w100"));
	assert(pw_copytype_driver("W100"));
	assert(pw_copytype_driver("imageon"));
	assert(pw_copytype_driver("IMAGEON"));
	assert(pw_copytype_driver("sa1100-lcdc"));
	assert(pw_copytype_driver("SA1100-LCDC"));
	assert(pw_copytype_driver("sa1100_lcdc"));
	assert(pw_copytype_driver("SA1100_LCDC"));
	assert(pw_copytype_driver("sa11x0-lcdc"));
	assert(pw_copytype_driver("SA11X0-LCDC"));
	assert(pw_copytype_driver("sa1100"));
	assert(pw_copytype_driver("SA1100"));

	/* Not copy-type */
	assert(!pw_copytype_driver("pxa-lcdc"));
	assert(!pw_copytype_driver("pxa2xx-lcdc"));
	assert(!pw_copytype_driver("unknown"));
	assert(!pw_copytype_driver(""));
	assert(!pw_copytype_driver(NULL));
	printf("✓ pw_copytype_driver\n");

	/* Test pw_copytype_resolve - all 3x2 override combinations */
	/* YES always true regardless of driver */
	assert(pw_copytype_resolve("mq11xx", PW_COPY_YES) == true);
	assert(pw_copytype_resolve("pxa-lcdc", PW_COPY_YES) == true);
	assert(pw_copytype_resolve(NULL, PW_COPY_YES) == true);

	/* NO always false regardless of driver */
	assert(pw_copytype_resolve("mq11xx", PW_COPY_NO) == false);
	assert(pw_copytype_resolve("pxa-lcdc", PW_COPY_NO) == false);
	assert(pw_copytype_resolve(NULL, PW_COPY_NO) == false);

	/* AUTO depends on driver */
	assert(pw_copytype_resolve("mq11xx", PW_COPY_AUTO) == true);
	assert(pw_copytype_resolve("w100", PW_COPY_AUTO) == true);
	assert(pw_copytype_resolve("sa1100-lcdc", PW_COPY_AUTO) == true);
	assert(pw_copytype_resolve("pxa-lcdc", PW_COPY_AUTO) == false);
	assert(pw_copytype_resolve("unknown", PW_COPY_AUTO) == false);
	assert(pw_copytype_resolve(NULL, PW_COPY_AUTO) == false);
	printf("✓ pw_copytype_resolve\n");

	/* Test pw_caching_parse */
	enum pw_caching_override co;
	assert(pw_caching_parse("auto", &co) && co == PW_CACHING_OV_AUTO);
	assert(pw_caching_parse("AUTO", &co) && co == PW_CACHING_OV_AUTO);
	assert(pw_caching_parse("cacheable", &co) && co == PW_CACHING_OV_CACHEABLE);
	assert(pw_caching_parse("Cacheable", &co) && co == PW_CACHING_OV_CACHEABLE);
	assert(pw_caching_parse("write_combined", &co) && co == PW_CACHING_OV_WC);
	assert(pw_caching_parse("WRITE_COMBINED", &co) && co == PW_CACHING_OV_WC);
	assert(!pw_caching_parse("", &co));
	assert(!pw_caching_parse("wc", &co));
	assert(!pw_caching_parse("yes", &co));
	assert(!pw_caching_parse(NULL, &co));
	assert(!pw_caching_parse("auto", NULL));
	printf("✓ pw_caching_parse\n");

	/* Test pw_caching_driver - the wire values are fixed by the protocol */
	assert(PW_CACHING_WRITE_COMBINED == 0 && PW_CACHING_CACHEABLE == 1);
	assert(pw_caching_driver("mq11xx") == PW_CACHING_CACHEABLE);
	assert(pw_caching_driver("MQ11XX") == PW_CACHING_CACHEABLE);
	assert(pw_caching_driver("mediaq") == PW_CACHING_CACHEABLE);
	assert(pw_caching_driver("MediaQ") == PW_CACHING_CACHEABLE);
	assert(pw_caching_driver("w100") == PW_CACHING_CACHEABLE);
	assert(pw_caching_driver("W100") == PW_CACHING_CACHEABLE);
	assert(pw_caching_driver("imageon") == PW_CACHING_CACHEABLE);
	assert(pw_caching_driver("ImageOn") == PW_CACHING_CACHEABLE);
	/* sa1100-lcdc is copy-type but CMA */
	assert(pw_caching_driver("sa1100-lcdc") == PW_CACHING_WRITE_COMBINED);
	assert(pw_caching_driver("SA1100_LCDC") == PW_CACHING_WRITE_COMBINED);
	assert(pw_caching_driver("sa11x0-lcdc") == PW_CACHING_WRITE_COMBINED);
	assert(pw_caching_driver("sa1100") == PW_CACHING_WRITE_COMBINED);
	assert(pw_caching_driver("pxa-lcdc") == PW_CACHING_WRITE_COMBINED);
	assert(pw_caching_driver("vc4") == PW_CACHING_WRITE_COMBINED);
	assert(pw_caching_driver("") == PW_CACHING_WRITE_COMBINED);
	assert(pw_caching_driver(NULL) == PW_CACHING_WRITE_COMBINED);

	/* every copy-type driver has an explicit decision: a test-local list
	 * mirroring pw_copytype_driver, with the expected caching of each */
	static const struct { const char *name; enum pw_caching c; } ct[] = {
		{ "mq11xx", PW_CACHING_CACHEABLE },
		{ "mediaq", PW_CACHING_CACHEABLE },
		{ "w100", PW_CACHING_CACHEABLE },
		{ "imageon", PW_CACHING_CACHEABLE },
		{ "sa1100-lcdc", PW_CACHING_WRITE_COMBINED },
		{ "sa1100_lcdc", PW_CACHING_WRITE_COMBINED },
		{ "sa11x0-lcdc", PW_CACHING_WRITE_COMBINED },
		{ "sa1100", PW_CACHING_WRITE_COMBINED },
	};
	for (unsigned i = 0; i < sizeof(ct) / sizeof(ct[0]); i++) {
		assert(pw_copytype_driver(ct[i].name));
		assert(pw_caching_driver(ct[i].name) == ct[i].c);
	}
	printf("✓ pw_caching_driver\n");

	/* Test pw_caching_resolve - the override wins over the table */
	assert(pw_caching_resolve("mq11xx", PW_CACHING_OV_AUTO) == PW_CACHING_CACHEABLE);
	assert(pw_caching_resolve("pxa-lcdc", PW_CACHING_OV_AUTO) == PW_CACHING_WRITE_COMBINED);
	assert(pw_caching_resolve(NULL, PW_CACHING_OV_AUTO) == PW_CACHING_WRITE_COMBINED);
	assert(pw_caching_resolve("mq11xx", PW_CACHING_OV_WC) == PW_CACHING_WRITE_COMBINED);
	assert(pw_caching_resolve("pxa-lcdc", PW_CACHING_OV_CACHEABLE) == PW_CACHING_CACHEABLE);
	assert(pw_caching_resolve(NULL, PW_CACHING_OV_CACHEABLE) == PW_CACHING_CACHEABLE);
	assert(pw_caching_resolve(NULL, PW_CACHING_OV_WC) == PW_CACHING_WRITE_COMBINED);

	/* names round-trip through the parser */
	assert(pw_caching_parse(pw_caching_name(PW_CACHING_CACHEABLE), &co) &&
	       co == PW_CACHING_OV_CACHEABLE);
	assert(pw_caching_parse(pw_caching_name(PW_CACHING_WRITE_COMBINED), &co) &&
	       co == PW_CACHING_OV_WC);
	printf("✓ pw_caching_resolve\n");

	return 0;
}
