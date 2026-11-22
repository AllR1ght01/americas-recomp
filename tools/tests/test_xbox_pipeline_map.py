import contextlib
import importlib.util
import io
import sys
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent

spec = importlib.util.spec_from_file_location("pipeline_map", TOOLS / "xbox" / "pipeline_map.py")
pm = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = pm
spec.loader.exec_module(pm)


class ExportModeTest(unittest.TestCase):
    def check(self, expected, names):
        for name in names:
            with self.subTest(name):
                self.assertEqual(pm.export_mode(name), expected)

    def test_register_values_match_spi_shader_col_format(self):
        self.assertEqual(
            [pm.ZERO, pm.R32, pm.GR32, pm.AR32, pm.FP16, pm.UN16, pm.SN16, pm.UI16, pm.SI16, pm.ABGR32],
            list(range(10)),
        )

    def test_32_bit_formats_use_the_32_bit_exports_by_channel_count(self):
        for kind in ("FLOAT", "UINT", "SINT"):
            self.check(pm.R32, [f"R32_{kind}"])
            self.check(pm.GR32, [f"R32G32_{kind}"])
            self.check(pm.ABGR32, [f"R32G32B32A32_{kind}"])

    def test_unsigned_integers_up_to_16_bits_export_as_uint16(self):
        self.check(pm.UI16, [
            "R16_UINT", "R16G16_UINT", "R16G16B16A16_UINT",
            "R8_UINT", "R8G8_UINT", "R8G8B8A8_UINT", "R10G10B10A2_UINT",
        ])

    def test_signed_integers_up_to_16_bits_export_as_sint16(self):
        self.check(pm.SI16, [
            "R16_SINT", "R16G16_SINT", "R16G16B16A16_SINT",
            "R8_SINT", "R8G8_SINT", "R8G8B8A8_SINT",
        ])

    def test_16_bit_normalized_formats_keep_their_16_bit_mode(self):
        self.check(pm.UN16, ["R16_UNORM", "R16G16_UNORM", "R16G16B16A16_UNORM"])
        self.check(pm.SN16, ["R16_SNORM", "R16G16_SNORM", "R16G16B16A16_SNORM"])

    def test_blendable_formats_export_as_fp16(self):
        self.check(pm.FP16, [
            "R16_FLOAT", "R16G16_FLOAT", "R16G16B16A16_FLOAT",
            "R8_UNORM", "R8G8_UNORM", "R8G8B8A8_UNORM", "B8G8R8A8_UNORM",
            "R8_SNORM", "R8G8_SNORM", "R8G8B8A8_SNORM",
            "R10G10B10A2_UNORM", "R11G11B10_FLOAT", "R9G9B9E5_SHAREDEXP",
            "B5G6R5_UNORM", "B5G5R5A1_UNORM", "B4G4R4A4_UNORM",
        ])

    def test_srgb_variants_match_their_linear_format(self):
        self.assertEqual(pm.export_mode("R8G8B8A8_UNORM_SRGB"), pm.export_mode("R8G8B8A8_UNORM"))
        self.assertEqual(pm.export_mode("B8G8R8A8_UNORM_SRGB"), pm.export_mode("B8G8R8A8_UNORM"))

    def test_dxgi_format_prefix_is_accepted(self):
        self.assertEqual(pm.export_mode("DXGI_FORMAT_R32G32_FLOAT"), pm.GR32)
        self.assertEqual(pm.export_mode("DXGI_FORMAT_R8G8B8A8_UNORM"), pm.FP16)

    def test_unknown_format_raises(self):
        with self.assertRaises(KeyError):
            pm.export_mode("R64_FLOAT")

    def test_every_listed_format_gets_a_color_target_mode(self):
        modes = {pm.export_mode(name) for name in pm._FORMATS}
        self.assertTrue(modes <= {pm.R32, pm.GR32, pm.FP16, pm.UN16, pm.SN16, pm.UI16, pm.SI16, pm.ABGR32})


class StageSettingsTest(unittest.TestCase):
    def test_export_mapping_is_the_identity_swizzle(self):
        self.assertEqual(pm.IDENTITY, 0xE4)
        for name in ("R8_UNORM", "R32G32_FLOAT", "R9G9B9E5_SHAREDEXP"):
            self.assertEqual(pm.export_mapping(name), pm.IDENTITY)

    def test_unbound_targets_export_zero_with_identity_swizzle(self):
        mode, mapping = pm.stage_settings([])
        self.assertEqual(mode, [pm.ZERO] * 8)
        self.assertEqual(mapping, [pm.IDENTITY] * 8)

    def test_bound_targets_land_at_their_own_index(self):
        mode, mapping = pm.stage_settings([None, "R32G32B32A32_FLOAT", None, "R16G16_UINT"])
        self.assertEqual(mode, [pm.ZERO, pm.ABGR32, pm.ZERO, pm.UI16, pm.ZERO, pm.ZERO, pm.ZERO, pm.ZERO])
        self.assertEqual(mapping, [pm.IDENTITY] * 8)

    def test_targets_past_the_eighth_are_ignored(self):
        mode, _ = pm.stage_settings(["R32_FLOAT"] * 10)
        self.assertEqual(mode, [pm.R32] * 8)

    def test_result_lists_are_always_eight_long(self):
        for formats in ([], ["R8_UNORM"], ["R8_UNORM"] * 8):
            mode, mapping = pm.stage_settings(formats)
            self.assertEqual((len(mode), len(mapping)), (8, 8))


class ColFormatRegisterTest(unittest.TestCase):
    def test_four_bits_per_target_starting_at_target_zero(self):
        self.assertEqual(pm.col_format_register(["R8G8B8A8_UNORM", "R32G32B32A32_FLOAT", None]), 0x94)

    def test_no_targets_is_zero(self):
        self.assertEqual(pm.col_format_register([]), 0)
        self.assertEqual(pm.col_format_register([None] * 8), 0)

    def test_eighth_target_uses_the_top_nibble(self):
        formats = [None] * 7 + ["R32G32B32A32_FLOAT"]
        self.assertEqual(pm.col_format_register(formats), pm.ABGR32 << 28)

    def test_register_fits_in_32_bits(self):
        self.assertLess(pm.col_format_register(["R32G32B32A32_FLOAT"] * 8), 1 << 32)


class SelfTestTest(unittest.TestCase):
    def test_module_self_test_passes_and_reports_the_format_count(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            pm.self_test()
        self.assertIn(f"{len(pm._FORMATS)} formats", out.getvalue())


if __name__ == "__main__":
    unittest.main()
