import sys
from pathlib import Path
import urllib.request


def send(script, url="http://127.0.0.1:5553/execute"):
    token = (Path.home() / "Documents/Vanta/.vanta-token").read_text().strip()
    request = urllib.request.Request(
        url, data=script.encode("utf-8"), method="POST",
        headers={"Content-Type": "text/plain; charset=utf-8",
                 "Authorization": f"Bearer {token}"})
    with urllib.request.urlopen(request, timeout=5) as response:
        return response.read().decode("utf-8", "replace")


if __name__ == "__main__":
    print(send(sys.stdin.read()))
