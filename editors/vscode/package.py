#!/usr/bin/env python3
"""editors/vscode/package.py: packages this folder as a VSCode extension, without npm or the network.

A .vsix is a zip: the extension's files under extension/, a manifest (extension.vsixmanifest) and the zip's content
types ([Content_Types].xml). Both are written from package.json, so a version bump there is the only edit needed.

    python3 editors/vscode/package.py               writes build-vscode/fminus-<version>.vsix (in the repository)
    python3 editors/vscode/package.py -o DIR        writes DIR/fminus-<version>.vsix
    code --install-extension build-vscode/fminus-0.6.0.vsix
"""
import argparse
import json
import pathlib
import zipfile
from xml.sax.saxutils import escape, quoteattr

HERE = pathlib.Path(__file__).resolve().parent
FILES = ["package.json", "language-configuration.json", "syntaxes/fminus.tmLanguage.json", "README.md"]

CONTENT_TYPES = """<?xml version="1.0" encoding="utf-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension=".json" ContentType="application/json"/>
  <Default Extension=".md" ContentType="text/markdown"/>
  <Default Extension=".vsixmanifest" ContentType="text/xml"/>
</Types>
"""


def manifest(package):
    return f"""<?xml version="1.0" encoding="utf-8"?>
<PackageManifest Version="2.0.0" xmlns="http://schemas.microsoft.com/developer/vsx-schema/2011">
  <Metadata>
    <Identity Language="en-US" Id={quoteattr(package["name"])} Version={quoteattr(package["version"])} Publisher={quoteattr(package["publisher"])}/>
    <DisplayName>{escape(package["displayName"])}</DisplayName>
    <Description xml:space="preserve">{escape(package["description"])}</Description>
    <Categories>{escape(",".join(package["categories"]))}</Categories>
    <Properties>
      <Property Id="Microsoft.VisualStudio.Code.Engine" Value={quoteattr(package["engines"]["vscode"])}/>
    </Properties>
  </Metadata>
  <Installation>
    <InstallationTarget Id="Microsoft.VisualStudio.Code"/>
  </Installation>
  <Dependencies/>
  <Assets>
    <Asset Type="Microsoft.VisualStudio.Code.Manifest" Path="extension/package.json" Addressable="true"/>
    <Asset Type="Microsoft.VisualStudio.Services.Content.Details" Path="extension/README.md" Addressable="true"/>
  </Assets>
</PackageManifest>
"""


def main():
    parser = argparse.ArgumentParser(description="Package the FMinus VSCode extension as a .vsix")
    parser.add_argument("-o", "--output-dir", type=pathlib.Path, default=HERE.parent.parent / "build-vscode")
    args = parser.parse_args()

    package = json.loads((HERE / "package.json").read_text())
    for name in FILES[:-1]:
        json.loads((HERE / name).read_text())   # fail here, not in VSCode, on a JSON mistake
    args.output_dir.mkdir(parents=True, exist_ok=True)
    vsix = args.output_dir / f"{package['name']}-{package['version']}.vsix"
    with zipfile.ZipFile(vsix, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("[Content_Types].xml", CONTENT_TYPES)
        archive.writestr("extension.vsixmanifest", manifest(package))
        for name in FILES:
            archive.write(HERE / name, f"extension/{name}")
    print(vsix)


if __name__ == "__main__":
    main()
