#!/usr/bin/env python3
import argparse
import email.utils
import html
from datetime import datetime, timezone
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description="Write the Sparkle appcast for a VoiceStick release.")
    parser.add_argument("--version", required=True)
    parser.add_argument("--zip-url", required=True)
    parser.add_argument("--signature", required=True)
    parser.add_argument("--length", required=True, type=int)
    parser.add_argument("--output", default="website/public/appcast.xml")
    parser.add_argument("--release-notes", default="VoiceStick macOS release.")
    args = parser.parse_args()

    notes = "".join(f"<li>{html.escape(line)}</li>" for line in args.release_notes.splitlines() if line.strip())
    if not notes:
        notes = "<li>VoiceStick macOS release.</li>"

    pub_date = email.utils.format_datetime(datetime.now(timezone.utc))
    content = f"""<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
  <channel>
    <title>VoiceStick</title>
    <link>https://78.github.io/voicestick/appcast.xml</link>
    <description>VoiceStick macOS updates</description>
    <language>zh-CN</language>
    <item>
      <title>Version {html.escape(args.version)}</title>
      <description><![CDATA[
        <ul>
          {notes}
        </ul>
      ]]></description>
      <pubDate>{pub_date}</pubDate>
      <sparkle:minimumSystemVersion>12.0</sparkle:minimumSystemVersion>
      <enclosure
        url="{html.escape(args.zip_url)}"
        sparkle:version="{html.escape(args.version)}"
        sparkle:shortVersionString="{html.escape(args.version)}"
        sparkle:edSignature="{html.escape(args.signature)}"
        length="{args.length}"
        type="application/octet-stream"
      />
    </item>
  </channel>
</rss>
"""
    Path(args.output).write_text(content, encoding="utf-8")


if __name__ == "__main__":
    main()
