#!/usr/bin/env python3
"""The one table that maps a build identity to its title id, name and data dir.

identities.json in this directory is the only place a console identity is
written down. It holds two entries — "dev" (the default build) and "release"
(RELEASE=1, the user's install) — each with:

  titleId            the homebrew id the package is installed as
  conceptId          its five digits
  contentId          the 36-character content id carrying the titleId
  titleName          what the console shows, and the uninstall ownership proof
  previousTitleNames older names the same id may still carry on the console
                     (the build an earlier version installed stays removable)
  dataDir            the one folder the title and its helper write on the console
  agentsMayRun       false for the user's install: no script in this repo may
                     launch it or read, list, write or delete its dataDir

Everything that needs one of these values reads it from here: build-title.mk
(package id and the compiled-in data root), platform/ps5/helper/Makefile (the
payload's data root), platform/ps5/scripts/ps5-env.sh (ids and data paths for
the shell scripts), title-uninstall.sh (the ownership proof) and
viewer/verify-package.sh (the package's titleName). No other file carries a
concrete id or data path.

A table the console or the guards would refuse is refused here: an id outside
PPSA9####, a content id that is not 36 characters carrying its titleId, a
concept id that is not that id's five digits, an empty title name, a data dir
that is not one folder directly under /data, two identities that share a data
dir, and one accepted title name claimed by two identities.

Usage:
  identity.py [--name NAME | --title-id ID]
      Print "titleId conceptId contentId dataDir" on one line (default "dev").
  identity.py --field FIELD [--name NAME | --title-id ID]
      Print one field: name, titleId, conceptId, contentId, titleName, dataDir
      or agentsMayRun (true/false).
  identity.py --param TEMPLATE --out FILE [--name NAME | --title-id ID]
      Write the package param.json: TEMPLATE with titleId, conceptId, contentId
      and titleName set (titleName for every language in localizedParameters).
  identity.py --table
      The uninstall allowlist, one "titleId<TAB>titleName<TAB>previous..." line
      per identity.
  identity.py --owns TITLE_ID
      Read one installed param.json from stdin and print "yes" or "no": that
      param.json carries TITLE_ID and one of that identity's own names. A
      foreign title that took an id (Twiso on PPSA99202) is "no".
  identity.py --private-data-dirs
      Every dataDir whose agentsMayRun is false, one per line: the folders no
      script in this repo may read, list, write or delete.

Exit: 0 printed/written (also for an ownership answer of "no"), 1 an unknown
identity or a value that fails the rules, 2 usage.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
IDENTITIES = os.path.join(HERE, "identities.json")

# titleId: four letters and five digits, in the 9xxxx homebrew range.
TITLE_ID_RE = re.compile(r"^PPSA9[0-9]{4}$")
# IV0000-PPSA99202_00-XXXXXXXXXXXXXXXX: 6 + 1 + 9 + 3 + 1 + 16 = 36 characters.
CONTENT_ID_RE = re.compile(r"^[A-Z]{2}[0-9]{4}-PPSA9[0-9]{4}_00-[A-Z0-9]{16}$")
# One folder directly under /data, e.g. /data/<project>-dev or /data/<project>.
DATA_DIR_RE = re.compile(r"^/data/[A-Za-z0-9][A-Za-z0-9._-]*$")

FIELDS = ("name", "titleId", "conceptId", "contentId", "titleName", "dataDir",
          "agentsMayRun")


def die(message):
    sys.stderr.write("identity.py: %s\n" % message)
    sys.exit(1)


def load(path=IDENTITIES):
    try:
        with open(path) as source:
            doc = json.load(source)
    except (OSError, ValueError) as error:
        die("cannot read %s: %s" % (path, error))
    check_doc(doc)
    return doc


def check(entry):
    """Refuse a table entry the console's loaders or the guards would refuse."""
    title_id = entry.get("titleId", "")
    concept_id = entry.get("conceptId", "")
    content_id = entry.get("contentId", "")
    title_name = entry.get("titleName", "")
    data_dir = entry.get("dataDir", "")
    if not TITLE_ID_RE.match(title_id):
        die("%s: titleId %r is not PPSA9 followed by four digits" % (entry, title_id))
    if concept_id != title_id[4:]:
        die("%s: conceptId %r is not the five digits of %s"
            % (entry, concept_id, title_id))
    if not CONTENT_ID_RE.match(content_id):
        die("%s: contentId %r is not IV0000-<titleId>_00-<16 chars>" % (entry, content_id))
    if len(content_id) != 36:
        die("%s: contentId %r is %d characters, not 36"
            % (entry, content_id, len(content_id)))
    if title_id not in content_id:
        die("%s: contentId %r does not carry the titleId" % (entry, content_id))
    if not title_name or title_name != title_name.strip():
        die("%s: titleName %r is empty or has surrounding spaces" % (entry, title_name))
    if not DATA_DIR_RE.match(data_dir):
        die("%s: dataDir %r is not one folder directly under /data (%s)"
            % (entry, data_dir, "/data/<name>"))
    if not isinstance(entry.get("agentsMayRun"), bool):
        die("%s: agentsMayRun must be true or false" % (entry,))
    names = entry.get("previousTitleNames", [])
    if not isinstance(names, list) or any(
            not isinstance(name, str) or not name or name != name.strip()
            for name in names):
        die("%s: previousTitleNames must be a list of non-empty names" % (entry,))
    return entry


def check_doc(doc):
    """Refuse a table whose identities would collide on the console."""
    titles = doc.get("titles", [])
    if not isinstance(titles, list) or not titles:
        die("%s holds no identity" % os.path.basename(IDENTITIES))
    ids, dirs, names = {}, {}, {}
    for entry in titles:
        check(entry)
        title_id = entry["titleId"]
        data_dir = entry["dataDir"]
        if title_id in ids:
            die("%s is listed twice" % title_id)
        if data_dir in dirs:
            die("%s and %s share the data dir %s; the two installs must not share data"
                % (dirs[data_dir], title_id, data_dir))
        for name in accepted_names(entry):
            if name in names:
                die("%s and %s both claim the title name %r; the uninstall proof "
                    "could not tell them apart" % (names[name], title_id, name))
            names[name] = title_id
        ids[title_id] = entry
        dirs[data_dir] = title_id
    default = doc.get("default", "dev")
    if default not in [entry.get("name") for entry in titles]:
        die("the default identity %r is not one of the titles (%s)"
            % (default, ", ".join(entry.get("name", "?") for entry in titles)))
    return doc


def accepted_names(entry):
    """Every titleName this identity may carry on the console."""
    return [entry["titleName"]] + list(entry.get("previousTitleNames", []))


def select(doc, name=None, title_id=None):
    titles = doc.get("titles", [])
    if title_id:
        for entry in titles:
            if entry.get("titleId") == title_id:
                return entry
        die("%s is not one of the identities in %s (%s); edit the table there"
            % (title_id, os.path.basename(IDENTITIES),
               ", ".join(entry.get("titleId", "?") for entry in titles)))
    want = name or doc.get("default", "dev")
    for entry in titles:
        if entry.get("name") == want:
            return entry
    die("unknown identity '%s' (known: %s)" % (want,
        ", ".join(entry.get("name", "?") for entry in titles)))


def table_lines(doc):
    """The uninstall allowlist: "titleId<TAB>titleName<TAB>previous..." lines."""
    lines = []
    for entry in doc.get("titles", []):
        lines.append("\t".join([entry["titleId"]] + accepted_names(entry)))
    return lines


def ownership(doc, title_id, param):
    """Does this installed param.json prove TITLE_ID is one of ours?

    "yes" when it carries that titleId and one of that identity's own names (its
    titleName or a previous one). "no" otherwise: an id in neither identity, a
    titleId that differs, or a titleName that is not one of that identity's — a
    foreign title such as Twiso on PPSA99202, or another identity's name.
    """
    if not isinstance(param, dict) or param.get("titleId") != title_id:
        return "no"
    localized = param.get("localizedParameters")
    shown = set()
    if isinstance(localized, dict):
        shown = {value["titleName"] for value in localized.values()
                 if isinstance(value, dict) and value.get("titleName")}
    for entry in doc.get("titles", []):
        if entry.get("titleId") == title_id:
            return "yes" if shown & set(accepted_names(entry)) else "no"
    return "no"


def private_data_dirs(doc):
    """Every dataDir no script in this repo may touch."""
    return [entry["dataDir"] for entry in doc.get("titles", [])
            if not entry.get("agentsMayRun")]


def field_value(doc, field, name=None, title_id=None):
    entry = select(doc, name, title_id)
    if field not in FIELDS:
        die("unknown field %r (known: %s)" % (field, ", ".join(FIELDS)))
    value = entry[field]
    if isinstance(value, bool):
        return "true" if value else "false"
    return value


def main():
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--name")
    parser.add_argument("--title-id")
    parser.add_argument("--field")
    parser.add_argument("--param")
    parser.add_argument("--out")
    parser.add_argument("--table", action="store_true")
    parser.add_argument("--owns")
    parser.add_argument("--private-data-dirs", action="store_true")
    try:
        args = parser.parse_args()
    except SystemExit:
        sys.stderr.write(__doc__)
        sys.exit(2)
    if args.name and args.title_id:
        die("pass --name or --title-id, not both")
    if bool(args.param) != bool(args.out):
        die("--param and --out are used together")
    if args.table:
        for line in table_lines(load()):
            print(line)
        return 0
    if args.private_data_dirs:
        for path in private_data_dirs(load()):
            print(path)
        return 0
    if args.owns:
        try:
            param = json.loads(sys.stdin.read())
        except ValueError:
            param = None
        print(ownership(load(), args.owns, param))
        return 0
    if args.field:
        print(field_value(load(), args.field, args.name, args.title_id))
        return 0

    entry = select(load(), args.name, args.title_id)

    if not args.param:
        print("%s %s %s %s" % (entry["titleId"], entry["conceptId"],
                               entry["contentId"], entry["dataDir"]))
        return 0

    try:
        with open(args.param) as source:
            doc = json.load(source)
    except (OSError, ValueError) as error:
        die("cannot read %s: %s" % (args.param, error))
    doc["titleId"] = entry["titleId"]
    doc["conceptId"] = entry["conceptId"]
    doc["contentId"] = entry["contentId"]
    localized = doc.get("localizedParameters")
    if not isinstance(localized, dict):
        localized = {}
        doc["localizedParameters"] = localized
    languages = [key for key, value in localized.items() if isinstance(value, dict)]
    if not languages:
        languages = ["en-US"]
        localized["en-US"] = {}
    localized.setdefault("defaultLanguage", "en-US")
    for language in languages:
        localized[language]["titleName"] = entry["titleName"]
    try:
        with open(args.out, "w") as out:
            json.dump(doc, out, indent=2, sort_keys=True)
            out.write("\n")
    except OSError as error:
        die("cannot write %s: %s" % (args.out, error))
    return 0


if __name__ == "__main__":
    sys.exit(main())
