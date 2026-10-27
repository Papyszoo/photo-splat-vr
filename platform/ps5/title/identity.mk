# The build identity, resolved once for every Makefile that builds something for
# the console: identities.json (this directory) is the only place an id, a title
# name or a data folder is written down, identity.py is the only reader, and this
# file is the only place the selection rules live.
#
# Included by build-title.mk (the viewer and every other title) and by
# platform/ps5/helper/Makefile (the capture payload the viewer ships and pushes),
# so a package and its helper always carry the same data folder. The shell
# scripts resolve the same three inputs with ps5_identity_selector in
# platform/ps5/scripts/ps5-env.sh.
#
# Inputs:
#   IDENTITY=<name>  any identity the table holds (dev, fresh, release)
#   RELEASE=1        the release identity (IDENTITY=release); naming another
#                    identity beside it is a contradiction, refused here
#   TITLE_ID=PPSA…   one identity by id (an input only: the table decides)
# Outputs: TITLE_ID, TITLE_CONCEPT_ID, TITLE_CONTENT_ID, TITLE_DATA_DIR,
#          TITLE_IDENTITY, TITLE_IDENTITY_SELECTOR, TITLE_IDENTITIES,
#          TITLE_IDENTITY_PY
#
# SPDX-License-Identifier: GPL-3.0-or-later

IDENTITY_MK_DIR    := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
TITLE_IDENTITIES   ?= $(IDENTITY_MK_DIR)/identities.json
TITLE_IDENTITY_PY  ?= $(IDENTITY_MK_DIR)/identity.py

# `make … RELEASE=1` selects the release identity and `make … IDENTITY=fresh`
# any identity by name; the dev build is the default.
TITLE_RELEASE      ?= $(if $(strip $(RELEASE)),$(strip $(RELEASE)),0)
TITLE_IDENTITY     ?= $(if $(strip $(IDENTITY)),$(strip $(IDENTITY)),$(if $(filter-out 0 no false,$(strip $(TITLE_RELEASE))),release,dev))
# RELEASE=1 already means the release identity: naming another one beside it is
# a contradiction, refused before anything is built.
ifneq ($(filter-out 0 no false,$(strip $(TITLE_RELEASE))),)
ifeq ($(filter release,$(strip $(TITLE_IDENTITY))),)
$(error RELEASE=$(TITLE_RELEASE) selects the release identity but IDENTITY=$(TITLE_IDENTITY) asks for another one; pass one of them, not both)
endif
endif
# A command-line TITLE_ID is an *input* only: the selector is built from it
# before the outputs are set, and the outputs are forced with `override` so the
# command-line value can never survive a failed lookup (W32 left `make
# TITLE_ID=PPSA12345` planning a build the table refuses; now it stops here).
TITLE_IDENTITY_SELECTOR := $(if $(strip $(TITLE_ID)),--title-id $(strip $(TITLE_ID)),--name $(TITLE_IDENTITY))

TITLE_IDENTITY_FIELDS := $(shell python3 $(TITLE_IDENTITY_PY) $(TITLE_IDENTITY_SELECTOR))
override TITLE_ID         := $(word 1,$(TITLE_IDENTITY_FIELDS))
override TITLE_CONCEPT_ID := $(word 2,$(TITLE_IDENTITY_FIELDS))
override TITLE_CONTENT_ID := $(word 3,$(TITLE_IDENTITY_FIELDS))
override TITLE_DATA_DIR   := $(word 4,$(TITLE_IDENTITY_FIELDS))
ifeq ($(TITLE_ID),)
$(error no title id: identity.py printed nothing for '$(TITLE_IDENTITY_SELECTOR)' (see its message above; identities.json holds the table))
endif
ifeq ($(TITLE_DATA_DIR),)
$(error no data dir: identity.py printed nothing for '$(TITLE_IDENTITY_SELECTOR)' (see its message above; identities.json holds the table))
endif
