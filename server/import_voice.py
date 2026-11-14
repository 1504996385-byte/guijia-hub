"""Import an explicitly provided local audio file into this service's voice library."""
import argparse
import base64
from pathlib import Path
from app import App, Config


def main():
    parser = argparse.ArgumentParser(description="导入本机语音库，不上传到云端")
    parser.add_argument("file", type=Path)
    parser.add_argument("--title", required=True)
    parser.add_argument("--language", default="普通话")
    parser.add_argument("--source", choices=("human", "test", "tts"), default="test")
    parser.add_argument("--data-dir", type=Path)
    args = parser.parse_args()
    config = Config.from_env()
    if args.data_dir:
        config.data_dir = args.data_dir
    voice, _ = App(config).add_voice({"audio_base64": base64.b64encode(args.file.read_bytes()).decode(),
                                    "title": args.title, "language": args.language, "voice_source": args.source})
    print(f"Imported voice: {voice['id']} ({voice['duration']:.1f}s); source={voice['voice_source']}")


if __name__ == "__main__":
    main()
