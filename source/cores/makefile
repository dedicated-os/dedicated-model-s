ifeq (,$(CROSS_COMPILE))
$(error missing CROSS_COMPILE, please run from the union-trimui-toolchain docker image)
endif

#######################################

CORES = gambatte gpsp pokemini

gambatte_REPO = https://github.com/libretro/gambatte-libretro
pokemini_REPO = https://github.com/libretro/PokeMini
pokemini_MAKEFILE = Makefile.libretro

#######################################

PATCH = git apply
PROCS = -j4
CORE_ARGS = platform=$(UNION_PLATFORM)

define TEMPLATE

$1_REPO ?= https://github.com/libretro/$(1)
$1_MAKE ?= make $(and $($1_MAKEFILE),-f $($1_MAKEFILE)) $(CORE_ARGS) $($(1)_FLAGS)
$1_BUILD_PATH ?= $(1)
$1_CORE ?= $(1)_libretro.so

.PHONY: $(1)

src/$(1):
	mkdir -p src
	cd src && git clone $(if $($1_HASH),,--depth 1) --recursive $$($(1)_REPO) $(1)
	$(if $($1_HASH),cd src/$$($1_BUILD_PATH) && git checkout $($1_HASH) && echo $($1_HASH),)

src/$(1)/.patched: src/$(1)
	(test ! -f patches/$(1).patch) || (test -f src/$(1)/.patched) || (cd src/$(1) && $(PATCH) -p1 < ../../patches/$(1).patch && touch .patched && true)
	
clone-$(1): src/$(1)
	
patch-$(1): src/$(1)/.patched

status-$(1): src/$(1)
	cd src/$(1) && git show --oneline -s

clean-$(1):
	test ! -d src/$(1) || cd src/$$($1_BUILD_PATH) && $$($1_MAKE) clean
	rm -rf build/$(1)_libretro.so

$(1): clone-$(1) patch-$(1)
	mkdir -p build
	cd src/$$($1_BUILD_PATH) && $$($1_MAKE) $(PROCS)
	cp src/$$($1_BUILD_PATH)/$$($(1)_CORE) ./build/$(1)_libretro.so

endef

#######################################

all: cores

$(foreach CORE,$(CORES),$(eval $(call TEMPLATE,$(CORE))))

list:
	@printf '\- %s\n' $(CORES)
	
cores: $(CORES)
	
clean: $(foreach CORE,$(CORES),clean-$(CORE))

nuke:
	rm -rf ./build
	rm -rf ./src
