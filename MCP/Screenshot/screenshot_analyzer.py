"""
MCP Screenshot Analyzer — takes URL screenshots via Playwright,
sends them directly to InferDeck gateway, returns model analysis.

Usage (MCP server — stdio):
    python screenshot_analyzer.py

Usage (standalone test):
    python screenshot_analyzer.py --test <url> [instruction]
    python screenshot_analyzer.py --test https://example.com "What color is the background?"
"""

import argparse
import base64
import datetime
import io
import logging
import os
import subprocess
import sys
import textwrap
import time
import traceback
from pathlib import Path

import httpx
from playwright.sync_api import Error as PlaywrightError, TimeoutError as PlaywrightTimeout
from playwright.sync_api import sync_playwright

try:
    from PIL import Image

    HAS_PIL = True
except ImportError:
    HAS_PIL = False

# ── Configuration (env-overridable) ──────────────────────────────────────────

# Script lives at .../MCP/Screenshot/screenshot_analyzer.py
SCRIPT_DIR = Path(__file__).resolve().parent
# Captures saved to .../MCP/Screenshot/Captures/<url>_<ts>.jpg
DEFAULT_CAPTURE_DIR = SCRIPT_DIR / "Captures"

# Gateway — defaults to the InferDeck Windows PC on LAN
INFERDECK_URL = os.environ.get(
    "INFERDECK_URL",
    "http://192.168.0.168:11434/api/chat",
)
MODEL = os.environ.get("INFERDECK_MODEL", "qwen3.6-35b-a3b")
GATEWAY_TIMEOUT = int(os.environ.get("INFERDECK_TIMEOUT", "120"))

# Screenshot limits
VIEWPORT_WIDTH = 1440
VIEWPORT_HEIGHT = 900
MAX_SCREENSHOT_PX = 1920 * 10800  # clamp anything wider/taller
JPEG_QUALITY = 85
PLAYWRIGHT_NAV_TIMEOUT_MS = 30_000
PLAYWRIGHT_SCREENSHOT_TIMEOUT_MS = 15_000

# Logging
logging.basicConfig(
    level=logging.INFO,
    format="[screenshot-analyzer] %(levelname)s: %(message)s",
)
log = logging.getLogger(__name__)

# ── Helpers ───────────────────────────────────────────────────────────────────


def _check_gateway() -> str:
    """Ping the gateway — returns OK or error message."""
    base = INFERDECK_URL.rsplit("/api/chat", 1)[0]
    url = f"{base}/api/tags"
    try:
        r = httpx.get(url, timeout=5)
        if r.status_code == 200:
            models = [m["name"] for m in r.json().get("models", [])]
            if MODEL in models:
                return f"Gateway OK (model '{MODEL}' found, {len(models)} models total)"
            else:
                return (
                    f"Gateway OK, but model '{MODEL}' NOT found. "
                    f"Available: {', '.join(models[:10])}{'...' if len(models) > 10 else ''}"
                )
        return f"Gateway at {base} returned HTTP {r.status_code}"
    except httpx.ConnectError:
        return f"CANNOT REACH gateway at {base} — is the Windows PC on and InferDeck running?"
    except httpx.TimeoutException:
        return f"Gateway at {base} timed out after 5s"
    except Exception as exc:
        return f"Gateway check failed: {type(exc).__name__}: {exc}"


def _take_screenshot(url: str) -> bytes:
    """Navigate to URL and capture full-page screenshot. Returns raw image bytes."""
    with sync_playwright() as pw:
        browser = pw.chromium.launch(headless=True)
        try:
            ctx = browser.new_context(
                viewport={"width": VIEWPORT_WIDTH, "height": VIEWPORT_HEIGHT},
                user_agent=(
                    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                    "AppleWebKit/537.36 (KHTML, like Gecko) "
                    "Chrome/125.0.0.0 Safari/537.36"
                ),
            )
            page = ctx.new_page()
            log.info("Navigating to %s ...", url)
            page.goto(url, wait_until="networkidle", timeout=PLAYWRIGHT_NAV_TIMEOUT_MS)
            log.info("Page loaded — taking screenshot ...")
            img_bytes = page.screenshot(
                full_page=True,
                timeout=PLAYWRIGHT_SCREENSHOT_TIMEOUT_MS,
            )
            log.info("Screenshot captured (%d bytes)", len(img_bytes))
            return img_bytes
        finally:
            browser.close()


def _maybe_resize(img_bytes: bytes) -> bytes:
    """If PIL is available, clamp oversized screenshots to keep POST body sane."""
    if not HAS_PIL:
        return img_bytes

    try:
        img = Image.open(io.BytesIO(img_bytes))
        w, h = img.size
        if w * h <= MAX_SCREENSHOT_PX:
            return img_bytes

        ratio = (MAX_SCREENSHOT_PX / (w * h)) ** 0.5
        new_w, new_h = int(w * ratio), int(h * ratio)
        log.warning("Screenshot oversized (%dx%d = %dpx) — resizing to %dx%d", w, h, w * h, new_w, new_h)
        img = img.resize((new_w, new_h), Image.LANCZOS)
        buf = io.BytesIO()
        img.save(buf, format="JPEG", quality=JPEG_QUALITY, optimize=True)
        log.info("Resized to %d bytes", buf.tell())
        return buf.getvalue()
    except Exception as exc:
        log.warning("Resize failed (%s) — using original", exc)
        return img_bytes


def _save_screenshot(img_bytes: bytes, url: str, save_dir: str | None = None) -> Path:
    """Save screenshot to disk, return the path."""
    dst = Path(save_dir) if save_dir else DEFAULT_CAPTURE_DIR
    dst.mkdir(parents=True, exist_ok=True)

    ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    safe_url = url.replace("://", "_").replace("/", "_")[:60]
    ext = ".jpg" if HAS_PIL else ".png"
    path = dst / f"{safe_url}_{ts}{ext}"

    # If we have PIL and image is JPEG-ready, save as JPEG (smaller)
    if HAS_PIL:
        img = Image.open(io.BytesIO(img_bytes))
        img.save(str(path), format="JPEG" if ext == ".jpg" else "PNG", quality=JPEG_QUALITY, optimize=True)
    else:
        path.write_bytes(img_bytes)

    log.info("Saved to %s (%d bytes)", path, path.stat().st_size)
    return path


def _analyze_with_gateway(img_bytes: str | bytes, instruction: str) -> str:
    """Send image + instruction to InferDeck, return the model's response text."""
    # Accept either raw bytes (auto-encode) or pre-encoded base64 string
    if isinstance(img_bytes, bytes):
        b64 = base64.b64encode(img_bytes).decode()
    else:
        b64 = img_bytes

    payload = {
        "model": MODEL,
        "messages": [{"role": "user", "content": instruction, "images": [b64]}],
        "stream": False,
    }

    log.info("Sending to InferDeck (%d chars of base64 + prompt) ...", len(b64))
    try:
        resp = httpx.post(
            INFERDECK_URL,
            json=payload,
            timeout=GATEWAY_TIMEOUT,
        )
    except httpx.ConnectError:
        return (
            f"ERROR: Cannot reach InferDeck gateway at {INFERDECK_URL}\n"
            f"  → Is the Windows PC on?\n"
            f"  → Is InferDeck running on port 11434?\n"
            f"  → Can the Mac ping 192.168.0.168?"
        )
    except httpx.TimeoutException:
        return (
            f"ERROR: Gateway at {INFERDECK_URL} timed out after {GATEWAY_TIMEOUT}s.\n"
            f"  → The model might still be loading.\n"
            f"  → Try a shorter prompt or check gateway logs."
        )
    except httpx.RequestError as exc:
        return f"ERROR: HTTP request failed — {type(exc).__name__}: {exc}"

    if resp.status_code != 200:
        return (
            f"ERROR: Gateway returned HTTP {resp.status_code}\n"
            f"  Body: {resp.text[:2000]}"
        )

    try:
        data = resp.json()
    except Exception as exc:
        return f"ERROR: Failed to parse gateway JSON response — {exc}\n  Raw: {resp.text[:2000]}"

    if "message" not in data or "content" not in data["message"]:
        return (
            f"ERROR: Unexpected gateway response format.\n"
            f"  Expected key 'message.content' but got keys: {list(data.keys())}\n"
            f"  Raw: {textwrap.shorten(str(data), width=2000)}"
        )

    content = data["message"]["content"]
    log.info("Gateway responded (%d chars)", len(content))
    return content


def _run_self_test(url: str, instruction: str) -> None:
    """Standalone test — validates the full pipeline without MCP."""
    print("=" * 60)
    print("  SCREENSHOT ANALYZER — SELF TEST")
    print("=" * 60)
    print(f"  URL:         {url}")
    print(f"  Instruction: {instruction}")
    print(f"  Gateway:     {INFERDECK_URL}")
    print(f"  Model:       {MODEL}")
    print(f"  Python:      {sys.version.split()[0]}")
    print(f"  PIL:         {'available' if HAS_PIL else 'NOT available (pip install Pillow)'}")
    print()

    # Step 1 — Gateway health
    print("[1/4] Checking gateway connectivity ...")
    status = _check_gateway()
    print(f"  → {status}")
    if "ERROR" in status or "CANNOT" in status:
        print("\nABORTING — gateway unreachable.")
        sys.exit(1)
    print()

    # Step 2 — Screenshot
    print("[2/4] Taking screenshot via Playwright ...")
    try:
        img_bytes = _take_screenshot(url)
    except PlaywrightTimeout as exc:
        print(f"  FAILED — page load timed out: {exc}")
        sys.exit(1)
    except PlaywrightError as exc:
        print(f"  FAILED — Playwright error: {exc}")
        sys.exit(1)
    except Exception as exc:
        print(f"  FAILED — {type(exc).__name__}: {exc}")
        sys.exit(1)
    print(f"  → Captured {len(img_bytes)} bytes")
    print()

    # Step 3 — Save + resize
    print("[3/4] Resizing (if needed) + saving ...")
    resized = _maybe_resize(img_bytes)
    path = _save_screenshot(resized, url)
    print(f"  → Saved to {path}")
    print()

    # Step 4 — Gateway analysis
    print(f"[4/4] Sending to InferDeck (timeout: {GATEWAY_TIMEOUT}s) ...")
    t0 = time.time()
    result = _analyze_with_gateway(resized, instruction)
    elapsed = time.time() - t0
    print(f"  → Responded in {elapsed:.1f}s")
    print()
    print("─" * 60)
    print("  MODEL RESPONSE:")
    print("─" * 60)
    print(result)
    print("─" * 60)
    print("  SELF TEST COMPLETE")

# ── MCP Tool ─────────────────────────────────────────────────────────────────


def build_mcp():
    from fastmcp import FastMCP

    mcp = FastMCP("screenshot-analyzer")

    @mcp.tool()
    def analyze_screenshot(
        url: str,
        instruction: str = "Describe this page in detail.",
        save_dir: str | None = None,
    ) -> str:
        """
        Take a full-page screenshot of a URL and analyze it using vision AI on InferDeck.

        Args:
            url: The full URL (including https://) to screenshot.
            instruction: What you want the vision model to analyze in the image.
            save_dir: Optional override directory to save the screenshot (default: MCP/Screenshot/Captures/).
        """
        # ── Pre-flight check ────────────────────────────────────────────────
        gateway_status = _check_gateway()
        log.info("Pre-flight: %s", gateway_status)

        if "Gateway OK" not in gateway_status:
            return (
                f"Pre-flight gateway check FAILED:\n  {gateway_status}\n\n"
                f"Make sure InferDeck is running on the Windows PC (192.168.0.168:11434) "
                f"and the Mac can reach it."
            )

        # ── Screenshot ──────────────────────────────────────────────────────
        log.info("Starting screenshot pipeline for: %s", url)
        try:
            img_bytes = _take_screenshot(url)
        except PlaywrightTimeout as exc:
            return (
                f"ERROR: Playwright timed out loading {url}\n"
                f"  {exc}\n\n"
                f"  Suggestions:\n"
                f"    • Check that the URL is correct and reachable from the Mac\n"
                f"    • The page might be too slow — try a simpler URL"
            )
        except PlaywrightError as exc:
            return (
                f"ERROR: Playwright failed to render {url}\n"
                f"  {exc}\n\n"
                f"  Suggestions:\n"
                f"    • The URL might not exist or might block headless browsers\n"
                f"    • Try a different URL"
            )
        except Exception as exc:
            return (
                f"ERROR: Unexpected failure during screenshot:\n"
                f"  {type(exc).__name__}: {exc}\n"
                f"  {traceback.format_exc()}"
            )

        # ── Resize (if oversized) ───────────────────────────────────────────
        try:
            img_bytes = _maybe_resize(img_bytes)
        except Exception as exc:
            log.warning("Resize failed, continuing with original: %s", exc)

        # ── Save to disk ────────────────────────────────────────────────────
        try:
            path = _save_screenshot(img_bytes, url, save_dir)
        except Exception as exc:
            log.warning("Save failed, continuing anyway: %s", exc)
            path = None

        # ── Analyze ─────────────────────────────────────────────────────────
        result = _analyze_with_gateway(img_bytes, instruction)

        # Append save location to result for traceability
        if path:
            result += f"\n\n(Screenshot saved to {path})"

        return result

    return mcp


# ── Entry point ──────────────────────────────────────────────────────────────

if __name__ == "__main__":
    log.info("screenshot-analyzer starting")
    log.info("  Script dir: %s", SCRIPT_DIR)
    log.info("  Gateway:    %s", INFERDECK_URL)
    log.info("  Model:      %s", MODEL)
    log.info("  Save dir:   %s", DEFAULT_CAPTURE_DIR)
    log.info("  PIL:        %s", "yes" if HAS_PIL else "no (install Pillow for resizing + JPEG)")

    parser = argparse.ArgumentParser(description="Screenshot Analyzer MCP Server")
    parser.add_argument("--test", nargs="?", const=None, default=None,
                        help="Run a self-test with a URL instead of starting the MCP server")
    parser.add_argument("instruction", nargs="?", default="Describe this page in detail.",
                        help="Instruction for the vision model (only with --test)")
    args = parser.parse_args()

    if args.test is not None:
        # --test was passed
        test_url = args.test
        if test_url is None:
            # No URL given with --test flag
            parser.print_help()
            print()
            print("Example:")
            print("  python screenshot_analyzer.py --test https://example.com")
            sys.exit(1)
        _run_self_test(url=test_url, instruction=args.instruction)
    else:
        # Normal MCP server mode
        mcp = build_mcp()
        mcp.run()
