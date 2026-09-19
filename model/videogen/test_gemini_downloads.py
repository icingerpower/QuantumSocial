"""Offline video-blob capture tests. Set QS_TEST_CHROME=1 for visible system Chrome."""
import base64
import html
import os
import tempfile
import unittest
from pathlib import Path

from playwright.sync_api import sync_playwright

from gemini_downloads import install_video_capture


class VideoBlobCaptureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.playwright = sync_playwright().start()
        system_chrome = os.environ.get("QS_TEST_CHROME") == "1"
        cls.browser = cls.playwright.chromium.launch(
            channel="chrome" if system_chrome else None, headless=not system_chrome)

    @classmethod
    def tearDownClass(cls):
        cls.browser.close()
        cls.playwright.stop()

    def setUp(self):
        self.context = self.browser.new_context(accept_downloads=True)
        self.page = self.context.new_page()
        self.directory = tempfile.TemporaryDirectory()
        self.state = install_video_capture(self.page, self.directory.name, lambda _: None)
        self.downloads = []
        self.page.on("download", lambda download: self.downloads.append(download))
        self.content = b"\x00\x00\x00\x18ftypmp42" + b"video fixture" * 10000

    def tearDown(self):
        self.context.close()
        self.directory.cleanup()

    def load_fixture(self, remove_frame=False):
        encoded = base64.b64encode(self.content).decode()
        inner = """<button id="download">Download video</button><script>
        download.onclick = () => {
            const bytes = Uint8Array.from(atob('%s'), c => c.charCodeAt(0));
            const a = document.createElement('a');
            a.href = URL.createObjectURL(new Blob([bytes], {type:'video/mp4'}));
            a.download = 'clip.mp4'; a.click(); URL.revokeObjectURL(a.href);
            parent.postMessage('remove-frame', '*');
        };</script>""" % encoded
        body = '<iframe sandbox="allow-scripts allow-downloads" srcdoc="' + html.escape(inner, quote=True) + '"></iframe>'
        if remove_frame:
            body += """<script>onmessage = e => {
                if (e.data === 'remove-frame') document.querySelector('iframe').remove();
            };</script>"""
        self.page.route("https://qs-test.example/**",
                        lambda route: route.fulfill(body=body, content_type="text/html"))
        self.page.goto("https://qs-test.example/")

    def wait_for_capture(self):
        for _ in range(40):
            if self.state["video"] or self.state["error"]:
                break
            self.page.wait_for_timeout(50)
        self.assertIsNone(self.state["error"])
        self.assertIsNotNone(self.state["video"])

    def test_sandbox_blob_saves_without_native_download(self):
        self.load_fixture()
        self.page.frame_locator("iframe").get_by_text("Download video").click()
        self.wait_for_capture()
        self.assertEqual(Path(self.state["video"]).read_bytes(), self.content)
        self.assertEqual(self.downloads, [])
        self.assertFalse(self.page.is_closed())

    def test_blob_survives_download_iframe_removal(self):
        self.load_fixture(remove_frame=True)
        self.page.frame_locator("iframe").get_by_text("Download video").click()
        self.wait_for_capture()
        self.assertEqual(self.page.locator("iframe").count(), 0)
        self.assertEqual(Path(self.state["video"]).read_bytes(), self.content)
        self.assertEqual(self.downloads, [])

    def test_next_job_uses_new_output_directory(self):
        self.load_fixture()
        self.page.frame_locator("iframe").get_by_text("Download video").click()
        self.wait_for_capture()
        first = self.state["video"]
        with tempfile.TemporaryDirectory() as second:
            self.state = install_video_capture(self.page, second, lambda _: None)
            self.page.reload()
            self.page.frame_locator("iframe").get_by_text("Download video").click()
            self.wait_for_capture()
            self.assertEqual(Path(self.state["video"]).parent, Path(second))
            self.assertEqual(Path(first).read_bytes(), self.content)

    def test_nonvideo_download_keeps_normal_behavior(self):
        self.page.route("https://qs-test.example/**", lambda route: route.fulfill(
            body="<a href='/image' download='image.png'>Image</a>", content_type="text/html"))
        self.page.route("https://qs-test.example/image", lambda route: route.fulfill(
            body=b"test image", headers={"Content-Disposition": 'attachment; filename="image.png"'}))
        self.page.goto("https://qs-test.example/")
        with self.page.expect_download() as download:
            self.page.get_by_text("Image", exact=True).click()
        self.assertEqual(download.value.suggested_filename, "image.png")
        self.assertIsNone(self.state["video"])

    def test_invalid_video_is_not_saved(self):
        self.page.evaluate("qsSaveVideoBlob('bad.mp4', 'video/mp4', 'bm90IGEgdmlkZW8=', null)")
        self.assertIsNone(self.state["video"])
        self.assertIn("MP4 or WebM", self.state["error"])
        self.assertEqual(list(Path(self.directory.name).iterdir()), [])


if __name__ == "__main__":
    unittest.main()
