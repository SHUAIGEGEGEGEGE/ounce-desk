"""Automated static audit & wire-protocol verification tests for OunceDesk (RDP 3389 + macOS VNC 5900)."""

from __future__ import annotations

import hashlib
import hmac
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC_DIR = ROOT / "client-switch2" / "source"


class TestSourceIntegrity(unittest.TestCase):
    def test_all_expected_cpp_sources_exist_and_non_empty(self) -> None:
        expected = [
            "main.cpp",
            "backend.hpp",
            "crypto_utils.hpp",
            "keymap.hpp",
            "rdp_backend.hpp",
            "rdp_backend.cpp",
            "vnc_backend.hpp",
            "vnc_backend.cpp",
            "profiles.hpp",
            "profiles.cpp",
            "input_mapper.hpp",
            "input_mapper.cpp",
            "ui_renderer.hpp",
            "ui_renderer.cpp",
            "font_bitmap.hpp",
            "platform.hpp",
            "platform_switch2.cpp",
            "platform_desktop.cpp",
            "wg_crypto.hpp",
            "wg_tunnel.hpp",
            "wg_tunnel.cpp",
        ]
        for name in expected:
            p = SRC_DIR / name
            self.assertTrue(p.exists(), f"Missing source file: {name}")
            self.assertGreater(p.stat().st_size, 200, f"Source file unexpectedly tiny: {name}")

    def test_all_local_includes_resolve(self) -> None:
        inc_re = re.compile(r'^\s*#include\s+"([^"]+)"', re.MULTILINE)
        for p in SRC_DIR.iterdir():
            if p.suffix not in (".hpp", ".cpp"):
                continue
            text = p.read_text(encoding="utf-8")
            for inc in inc_re.findall(text):
                self.assertTrue((SRC_DIR / inc).exists(), f"{p.name} includes missing local header '{inc}'")

    def test_no_cpp_exceptions_or_rtti_used_with_fno_exceptions(self) -> None:
        # Makefile compiles with -fno-exceptions -fno-rtti
        bad_re = re.compile(r"\b(try\s*\{|catch\s*\(|throw\b|dynamic_cast\b|typeid\b)")
        for p in SRC_DIR.iterdir():
            if p.suffix not in (".hpp", ".cpp"):
                continue
            text = p.read_text(encoding="utf-8")
            # Strip line comments
            cleaned = re.sub(r"//.*", "", text)
            self.assertIsNone(bad_re.search(cleaned), f"{p.name} uses exceptions/RTTI incompatible with -fno-exceptions")

    def test_strictly_ascii_filenames_for_kaihen_compatibility(self) -> None:
        for p in ROOT.rglob("*"):
            if "__pycache__" in p.parts:
                continue
            self.assertTrue(p.name.isascii(), f"Non-ASCII filename breaks kaiHEN OunceShell/FTP: {p}")

    def test_switch2_icon_is_valid_256x256_baseline_jpeg(self) -> None:
        icon = ROOT / "client-switch2" / "icon.jpg"
        self.assertTrue(icon.exists(), "Missing client-switch2/icon.jpg")
        raw = icon.read_bytes()
        self.assertTrue(raw.startswith(b"\xff\xd8"), "icon.jpg is not a valid JPEG")
        try:
            from PIL import Image
            with Image.open(icon) as im:
                self.assertEqual(im.size, (256, 256))
                self.assertEqual(im.format, "JPEG")
        except ImportError:
            pass


class TestProtocolConstants(unittest.TestCase):
    def test_rdp_and_vnc_implement_required_auth_and_codecs(self) -> None:
        rdp_src = (SRC_DIR / "rdp_backend.cpp").read_text(encoding="utf-8")
        vnc_src = (SRC_DIR / "vnc_backend.cpp").read_text(encoding="utf-8")
        crypto_src = (SRC_DIR / "crypto_utils.hpp").read_text(encoding="utf-8")

        # Windows 3389 RDP requirements: X.224, TLS, CredSSP/NLA NTLMv2, MCS, FastPath, RLE, DeactivateAll
        for token in (
            "do_x224_negotiation",
            "upgrade_to_tls",
            "do_credssp_nla",
            "do_mcs_connect",
            "do_client_info_pdu",
            "handle_demand_active",
            "decompress_interleaved", "decompress_planar",
            "PDUTYPE_DEACTIVATEALLPDU",
            "FASTPATH_UPDATETYPE_BITMAP",
            "FASTPATH_INPUT_EVENT_MOUSE",
        ):
            self.assertIn(token, rdp_src, f"RDP backend missing {token}")

        # macOS 5900 VNC requirements: RFB 3.8, Apple ARD (30), VNC Auth (2), ZRLE (16), CopyRect (1), DesktopSize (-223)
        for token in (
            "RFB 003.008",
            "apple_ard_encrypt_credentials",
            "vnc_des_encrypt_challenge",
            "decode_zrle_rect",
            "decode_copyrect",
            "decode_raw_rect",
            "-223",
        ):
            self.assertIn(token, vnc_src, f"VNC backend missing {token}")

        # Crypto primitives: MD4, MD5, HMAC-MD5, RC4, VNC bit-reversed DES, Apple ARD DH+AES
        for token in ("md4(", "md5(", "hmac_md5(", "RC4State", "vnc_des_encrypt_challenge", "apple_ard_encrypt_credentials"):
            self.assertIn(token, crypto_src, f"crypto_utils.hpp missing {token}")

    def test_makefile_uses_libounce_and_standard_portlibs(self) -> None:
        mk = (ROOT / "client-switch2" / "Makefile").read_text(encoding="utf-8")
        self.assertIn("include $(DEVKITPRO)/libounce/switch2_rules", mk)
        for lib in ("-lSDL2", "-lmbedtls", "-lmbedx509", "-lmbedcrypto", "-lz", "-lEGL", "-lnx"):
            self.assertIn(lib, mk, f"Makefile missing library {lib}")


if __name__ == "__main__":
    unittest.main()
