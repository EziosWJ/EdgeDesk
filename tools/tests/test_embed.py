import importlib.util
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "gen_embed.py"
spec = importlib.util.spec_from_file_location("gen_embed", SCRIPT)
gen_embed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen_embed)


class EmbedGeneratorTests(unittest.TestCase):
    def test_registry_has_sorted_index_and_embed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "dist"
            (root / "assets").mkdir(parents=True)
            (root / "index.html").write_text("<h1>ok</h1>", encoding="utf-8")
            (root / "assets" / "x.js").write_bytes(b"console.log(1);\x00")
            out = Path(temp) / "out.cpp"
            self.assertEqual(gen_embed.generate(root, out), 2)
            text = out.read_text()
            self.assertIn('#embed "', text)
            self.assertIn(' suffix(,)', text)
            self.assertIn('"/assets/x.js"', text)
            self.assertLess(text.index('"/assets/x.js"'), text.index('"/index.html"'))
            self.assertIn('sizeof(kData0)-1', text)
            before = out.stat().st_mtime_ns
            gen_embed.generate(root, out)
            self.assertEqual(before, out.stat().st_mtime_ns)

    def test_requires_index(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "dist"
            root.mkdir()
            with self.assertRaises(ValueError):
                gen_embed.generate(root, Path(temp) / "out.cpp")

    def test_rejects_symlink(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "dist"
            root.mkdir()
            (root / "index.html").write_text("ok")
            (root / "bad").symlink_to(root / "index.html")
            with self.assertRaises(ValueError):
                gen_embed.generate(root, Path(temp) / "out.cpp")


if __name__ == "__main__":
    unittest.main()
