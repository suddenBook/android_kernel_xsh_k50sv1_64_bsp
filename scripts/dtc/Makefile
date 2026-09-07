# scripts/dtc makefile

hostprogs-y	:= dtc dtc_overlay ufdt_apply_overlay
always		:= $(hostprogs-y)

dtc-objs	:= dtc.o flattree.o fstree.o data.o livetree.o treesource.o \
		   srcpos.o checks.o util.o
dtc-objs	+= dtc-lexer.lex.o dtc-parser.tab.o

# Source files need to get at the userspace version of libfdt_env.h to compile

HOSTCFLAGS_DTC := -I$(src) -I$(src)/libfdt

HOSTCFLAGS_checks.o := $(HOSTCFLAGS_DTC)
HOSTCFLAGS_data.o := $(HOSTCFLAGS_DTC)
HOSTCFLAGS_dtc.o := $(HOSTCFLAGS_DTC)
HOSTCFLAGS_flattree.o := $(HOSTCFLAGS_DTC)
HOSTCFLAGS_fstree.o := $(HOSTCFLAGS_DTC)
HOSTCFLAGS_livetree.o := $(HOSTCFLAGS_DTC)
HOSTCFLAGS_srcpos.o := $(HOSTCFLAGS_DTC)
HOSTCFLAGS_treesource.o := $(HOSTCFLAGS_DTC)
HOSTCFLAGS_util.o := $(HOSTCFLAGS_DTC)

HOSTCFLAGS_dtc-lexer.lex.o := $(HOSTCFLAGS_DTC)
HOSTCFLAGS_dtc-parser.tab.o := $(HOSTCFLAGS_DTC)

# dependencies on generated files need to be listed explicitly
$(obj)/dtc-lexer.lex.o: $(obj)/dtc-parser.tab.h

# generated files need to be cleaned explicitly
clean-files	:= dtc-lexer.lex.c dtc-parser.tab.c dtc-parser.tab.h

# The MTK DTB rules need symbol/fixup support beyond the kernel's original DTC.
# Build the pinned AOSP tools in objtree; no executable is supplied by srctree.
dtc_overlay-objs := $(addprefix aosp/dtc/, \
	checks.o data.o dtc.o flattree.o fstree.o livetree.o srcpos.o \
	treesource.o util.o dtc-lexer.lex.o dtc-parser.tab.o)

ufdt_apply_overlay-objs := $(addprefix aosp/libufdt/, \
	ufdt_overlay.o ufdt_convert.o ufdt_node.o ufdt_node_pool.o \
	ufdt_prop_dict.o sysdeps/libufdt_sysdeps_posix.o \
	tests/src/ufdt_overlay_test_app.o tests/src/util.o)
ufdt_apply_overlay-objs += $(addprefix aosp/dtc/libfdt/, \
	fdt.o fdt_ro.o fdt_wip.o fdt_sw.o fdt_rw.o fdt_strerror.o \
	fdt_empty_tree.o fdt_addresses.o)

overlay-host-objs := $(addprefix $(obj)/,$(dtc_overlay-objs) $(ufdt_apply_overlay-objs))
$(overlay-host-objs): HOST_EXTRACFLAGS += -std=gnu99 \
	-I$(abspath $(obj)/aosp/dtc) -I$(src)/aosp/dtc -I$(src)/aosp/dtc/libfdt \
	-I$(src)/aosp/libufdt/include -I$(src)/aosp/libufdt/sysdeps/include

# Generate in objtree without the legacy REGENERATE_PARSERS/shipped-C path.
HOSTLEX ?= flex
HOSTYACC ?= bison
overlay_bison_env = $(if $(BISON_PKGDATADIR),BISON_PKGDATADIR=$(BISON_PKGDATADIR))
quiet_cmd_overlay_lex = LEX     $@
      cmd_overlay_lex = $(HOSTLEX) -o$@ -L $<
quiet_cmd_overlay_yacc = YACC    $@
      cmd_overlay_yacc = $(overlay_bison_env) $(HOSTYACC) -o$@ -t -l $<
quiet_cmd_overlay_yacc_h = YACC    $@
      cmd_overlay_yacc_h = $(overlay_bison_env) $(HOSTYACC) -o/dev/null --defines=$@ -t -l $<

$(obj)/aosp/dtc/dtc-lexer.lex.c: $(src)/aosp/dtc/dtc-lexer.l FORCE
	$(call if_changed,overlay_lex)
$(obj)/aosp/dtc/dtc-parser.tab.c: $(src)/aosp/dtc/dtc-parser.y FORCE
	$(call if_changed,overlay_yacc)
# AOSP's lexer includes dtc-parser.h, rather than the older dtc-parser.tab.h.
$(obj)/aosp/dtc/dtc-lexer.lex.o: $(obj)/aosp/dtc/dtc-parser.h
$(obj)/aosp/dtc/dtc-parser.h: $(src)/aosp/dtc/dtc-parser.y FORCE
	$(call if_changed,overlay_yacc_h)

targets += aosp/dtc/dtc-lexer.lex.c aosp/dtc/dtc-parser.tab.c aosp/dtc/dtc-parser.h
clean-files += aosp/dtc/dtc-lexer.lex.c aosp/dtc/dtc-parser.tab.c \
	aosp/dtc/dtc-parser.h
