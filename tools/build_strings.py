# Turns en.json into data/strings.js, and keeps the two frontend files honest.
#
# en.json is the one maintained source for user-facing text. Only the generated
# file is packed into the filesystem image, so the catalog never occupies flash
# twice and the page never has to fetch it.
#
# The checks run with the generation rather than as a separate step, because a
# key the catalog does not answer reaches the user as a fallback word: the page
# still reads as English, but it no longer says what it was written to say.
import json
import os
import re
import sys

# PlatformIO runs this through SCons, which defines no __file__; a plain
# "python tools/build_strings.py" has no SCons env. Ask whichever is present,
# so the generated file is there for the filesystem image and for
# ui-mock-server.js alike.
try:
    Import("env")  # noqa: F821  - SCons injects env into this namespace
    REPOSITORY = env.subst("$PROJECT_DIR")  # noqa: F821
except NameError:
    REPOSITORY = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CATALOG = os.path.join(REPOSITORY, "en.json")
GENERATED = os.path.join(REPOSITORY, "data", "strings.js")
MARKUP = os.path.join(REPOSITORY, "data", "index.html")
SCRIPT = os.path.join(REPOSITORY, "data", "app.js")

# A catalog key as it is written in both frontend files.
KEY = re.compile(r"^[a-z][A-Za-z0-9]*(?:\.[A-Za-z0-9]+)+$")

# Text the frontend owns but must not translate: the product and brand names,
# the serial framings and Enter line endings, the unit symbols beside a number,
# and the example host.
ALLOWED_MARKUP_TEXT = {
    "Serial2WiFi",
    "synapse.sr",
    "server.example.com",
    "8N1", "8N2", "8E1", "8O1", "7E1", "7O1",
    "CRLF", "CR", "LF",
    "ms", "sec",
}

# Literals in app.js that no one reads: a media query, and the names three
# failed requests give themselves for the catch that discards them.
ALLOWED_SCRIPT_TEXT = {
    "(prefers-color-scheme: dark)",
    "trial unavailable",
    "scan failed",
    "status unavailable",
    "settings unavailable",
}


def flatten(node, prefix=""):
    # Keys beginning with _ document the catalog and are not text.
    flat = {}
    for name, value in node.items():
        if name.startswith("_"):
            continue
        key = f"{prefix}{name}"
        if isinstance(value, dict):
            flat.update(flatten(value, f"{key}."))
        else:
            flat[key] = value
    return flat


def read(path):
    with open(path, encoding="utf-8") as source:
        return source.read()


def markupWithoutNoise(markup):
    # The icon sprite is Lucide path data, and the head script sets the theme
    # before the first paint. Neither carries product text.
    markup = re.sub(r"<svg class=\"icon-sprite\".*?</svg>", "", markup, flags=re.S)
    markup = re.sub(r"<script.*?</script>", "", markup, flags=re.S)
    return re.sub(r"<!--.*?-->", "", markup, flags=re.S)


def withoutInterpolation(template):
    # Only the authored parts of a template literal are text; ${...} is code.
    while True:
        shorter = re.sub(r"\$\{[^{}]*\}", "", template)
        if shorter == template:
            return template
        template = shorter


def scriptLiterals(script):
    # A small scanner rather than a regex: app.js comments contain apostrophes,
    # and a quote-matching pattern reads those as the start of a literal.
    literals = []
    index = 0
    line = 1
    length = len(script)
    while index < length:
        character = script[index]
        if character == "\n":
            line += 1
            index += 1
        elif script.startswith("//", index):
            index = script.find("\n", index)
            if index == -1:
                break
        elif script.startswith("/*", index):
            end = script.find("*/", index)
            end = length if end == -1 else end + 2
            line += script.count("\n", index, end)
            index = end
        elif character in "'\"`":
            start = index
            index += 1
            while index < length and script[index] != character:
                index += 2 if script[index] == "\\" else 1
            text = script[start + 1:index]
            if character == "`":
                text = withoutInterpolation(text)
            literals.append((line, text))
            line += script.count("\n", start, index)
            index += 1
        else:
            index += 1
    return literals


def looksLikeProse(text):
    return " " in text and len(re.findall(r"[A-Za-z]{2,}", text)) >= 2


def checkMarkup(markup, catalog, used, problems):
    body = markupWithoutNoise(markup)
    for key in re.findall(r"data-i18n(?:-label|-placeholder)?=\"([^\"]*)\"", body):
        used.add(key)
        if key not in catalog:
            problems.append(f"data/index.html: no catalog entry for {key}")
    for text in re.findall(r">([^<>]+)<", body):
        stripped = text.strip()
        if re.search(r"[A-Za-z]{2,}", stripped) and stripped not in ALLOWED_MARKUP_TEXT:
            problems.append(f"data/index.html: text outside the catalog: {stripped!r}")
    for _, text in re.findall(r"(?<=\s)(aria-label|placeholder|title|alt)=\"([^\"]*)\"", body):
        if re.search(r"[A-Za-z]{2,}", text) and text not in ALLOWED_MARKUP_TEXT:
            problems.append(f"data/index.html: attribute text outside the catalog: {text!r}")


def checkScript(script, catalog, used, problems):
    for line, text in scriptLiterals(script):
        if KEY.match(text):
            used.add(text)
            if text not in catalog:
                problems.append(f"data/app.js:{line}: no catalog entry for {text}")
        elif looksLikeProse(text) and text not in ALLOWED_SCRIPT_TEXT:
            problems.append(f"data/app.js:{line}: text outside the catalog: {text!r}")


def build():
    catalog = flatten(json.loads(read(CATALOG)))
    used = set()
    problems = []
    checkMarkup(read(MARKUP), catalog, used, problems)
    checkScript(read(SCRIPT), catalog, used, problems)
    for key in sorted(set(catalog) - used):
        problems.append(f"en.json: {key} is not used by the frontend")

    if problems:
        print("en.json and the frontend disagree:", file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        sys.exit(1)

    with open(GENERATED, "w", encoding="utf-8", newline="\n") as generated:
        generated.write("// Generated by tools/build_strings.py from en.json. Do not edit.\n")
        generated.write("window.strings = ")
        generated.write(json.dumps(catalog, ensure_ascii=False, separators=(",", ":")))
        generated.write(";\n")


build()
