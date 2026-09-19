"""Local browser regressions; never connect to Gemini or a user profile.

Run: python3 -m unittest discover -s model/videogen -p 'test_gemini_submission.py'
Requires Playwright and its Chromium browser.
"""
import base64
import io
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

from playwright.sync_api import sync_playwright

import generate_video_gemini as worker


class GeminiSubmissionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.playwright = sync_playwright().start()
        cls.browser = cls.playwright.chromium.launch(headless=True)

    @classmethod
    def tearDownClass(cls):
        cls.browser.close()
        cls.playwright.stop()

    def setUp(self):
        self.page = self.browser.new_page()
        self.log_patch = patch.object(worker, "_log")
        self.log_patch.start()

    def tearDown(self):
        self.page.close()
        self.log_patch.stop()

    def composer(self, body="<textarea aria-label='Prompt'></textarea>", script=""):
        self.page.set_content(body + """
            <button id='send' aria-label='Envoyer'>Send</button>
            <script>window.clicks = 0;
            send.onclick = () => { window.clicks++; };
            </script>""" + "<script>" + script + "</script>")
        self.editor = self.page.get_by_role("textbox")
        self.editor.fill("A test video")

    def submit(self, **kwargs):
        return worker._submit_prompt(self.page, self.editor, "A test video",
                                     timeout_s=1.5, retry_interval_s=0.5, **kwargs)

    def test_textarea_ignored_click_is_not_success(self):
        self.composer()
        self.assertFalse(self.submit())
        self.assertEqual(self.page.evaluate("window.clicks"), 3)
        self.assertEqual(self.editor.input_value(), "A test video")

    def test_retries_ignored_first_click(self):
        self.composer(script="""
            send.onclick = () => {
                if (++window.clicks === 2) document.querySelector('textarea').value = '';
            };""")
        self.assertTrue(self.submit())
        self.assertEqual(self.page.evaluate("window.clicks"), 2)

    def test_waits_until_enabled(self):
        self.composer(script="""
            send.disabled = true;
            setTimeout(() => send.disabled = false, 400);
            send.onclick = () => {
                window.clicks++; document.querySelector('textarea').value = '';
            };""")
        self.assertTrue(self.submit())
        self.assertEqual(self.page.evaluate("window.clicks"), 1)

    def test_skips_hidden_send_button(self):
        self.composer(script="""
            const hidden = document.createElement('button');
            hidden.setAttribute('aria-label', 'Envoyer'); hidden.hidden = true;
            document.body.prepend(hidden);
            send.onclick = () => {
                window.clicks++; document.querySelector('textarea').value = '';
            };""")
        self.assertTrue(self.submit())
        self.assertEqual(self.page.evaluate("window.clicks"), 1)

    def test_contenteditable_clears_asynchronously_without_duplicate(self):
        self.composer('<div role="textbox" contenteditable="true"></div>', """
            send.onclick = () => {
                window.clicks++;
                setTimeout(() => document.querySelector('[role=textbox]').innerText = '', 300);
            };""")
        self.assertTrue(self.submit())
        self.assertEqual(self.page.evaluate("window.clicks"), 1)

    def test_contenteditable_blank_lines_do_not_block_submission(self):
        self.composer('<div role="textbox" contenteditable="true"></div>', """
            send.onclick = () => {
                window.clicks++;
                document.querySelector('[role=textbox]').innerText = '';
            };""")
        prompt = "Known pitfalls:\n- Keep the shoes consistent.\n\nThe model walks."
        self.editor.fill(prompt)
        self.assertNotEqual(self.editor.evaluate('el => el.innerText'), prompt)
        self.assertTrue(worker._submit_prompt(
            self.page, self.editor, prompt, timeout_s=1.5, retry_interval_s=0.5))
        self.assertEqual(self.page.evaluate("window.clicks"), 1)

    def test_contenteditable_changed_words_still_block_submission(self):
        self.composer('<div role="textbox" contenteditable="true"></div>')
        self.editor.fill("A different video")
        self.assertFalse(self.submit())
        self.assertEqual(self.page.evaluate("window.clicks"), 0)

    def test_detached_editor_is_not_success(self):
        self.composer(script="send.onclick = () => document.querySelector('textarea').remove();")
        self.assertFalse(self.submit())

    def test_missing_attachment_blocks_send(self):
        self.composer()
        self.assertFalse(self.submit(required_attachments=1))
        self.assertEqual(self.page.evaluate("window.clicks"), 0)

    def upload_fixture(self, preview=True):
        # A genuine local file chooser and asynchronous preview, no network.
        self.page.set_content("""
            <button onclick="document.querySelector('input').click()">Importer des fichiers</button>
            <input type="file" hidden>
            <file-preview></file-preview>
            """ + ("""<script>
            document.querySelector('input').onchange = event => {
                const url = URL.createObjectURL(event.target.files[0]);
                setTimeout(() => {
                    const img = new Image(); img.src = url;
                    document.querySelector('file-preview').append(img);
                }, 300);
            };</script>""" if preview else ""))

    def image_file(self, directory):
        path = Path(directory) / "image.png"
        path.write_bytes(base64.b64decode(
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aF1sAAAAASUVORK5CYII="))
        return str(path)

    def test_upload_waits_for_loaded_preview(self):
        self.upload_fixture()
        with tempfile.TemporaryDirectory() as directory:
            self.assertTrue(worker._attach_image(self.page, self.image_file(directory), timeout_s=2))
        self.assertEqual(worker._attachment_count(self.page), 1)

    def test_file_chooser_without_preview_is_not_success(self):
        self.upload_fixture(preview=False)
        with tempfile.TemporaryDirectory() as directory:
            self.assertFalse(worker._attach_image(self.page, self.image_file(directory), timeout_s=0.5))

    def test_history_and_busy_previews_do_not_count(self):
        self.upload_fixture()
        with tempfile.TemporaryDirectory() as directory:
            self.assertTrue(worker._attach_image(self.page, self.image_file(directory), timeout_s=2))
        self.page.evaluate("""() => {
            const history = document.createElement('user-query');
            history.append(document.querySelector('file-preview').cloneNode(true));
            document.body.append(history);
            const busy = document.createElement('span');
            busy.setAttribute('role', 'progressbar');
            document.querySelector('file-preview').append(busy);
        }""")
        self.assertEqual(worker._attachment_count(self.page), 0)

    def test_failed_upload_stops_attempt_before_submission(self):
        with patch.object(worker, "_video_quota_message", return_value=None), \
                patch.object(worker, "_select_video_tool", return_value=True), \
                patch.object(worker, "_select_aspect") as aspect, \
                patch.object(worker, "_attach_image", return_value=False) as attach, \
                patch.object(worker, "_submit_prompt") as submit, \
                patch.object(worker, "_dump_state"):
            attach.side_effect = lambda *args: aspect.assert_called_once() or False
            status, error = worker._run_attempt(
                self.page, "test", ["image.png"], "/unused", time.time() + 20,
                time.time() + 20, 10)
        self.assertEqual(status, "error")
        self.assertIn("not sent", error)
        submit.assert_not_called()

    def test_missing_requested_image_is_not_silently_discarded(self):
        with self.assertRaisesRegex(ValueError, "image file is missing"):
            worker._read_request({"images": ["/nonexistent/image.png"]})

    def test_unconfirmed_submission_does_not_wait_for_generation(self):
        self.composer()
        with patch.object(worker, "_video_quota_message", return_value=None), \
                patch.object(worker, "_select_video_tool", return_value=True), \
                patch.object(worker, "_select_aspect"), \
                patch.object(worker, "_submit_prompt", return_value=False), \
                patch.object(worker, "_dump_state") as dump, \
                patch.object(worker.time, "sleep") as sleep:
            status, error = worker._run_attempt(
                self.page, "test", [], "/unused", time.time() + 20,
                time.time() + 20, 10)
        self.assertEqual(status, "error")
        self.assertIn("submission", error)
        dump.assert_called_once_with(self.page, "submission_failed")
        sleep.assert_not_called()

    def test_repeat_does_not_rewrite_or_resubmit_rejected_prompt(self):
        page = Mock()
        with tempfile.TemporaryDirectory() as directory, \
                patch.object(worker, "_run_attempt", return_value=("rejected", "refused")) as run:
            result, _ = worker._generate_once(None, page, "Exact prompt.", [], directory,
                                              {"preservePrompt": True})
        self.assertTrue(result["rejected"])
        self.assertEqual(run.call_count, 1)
        self.assertEqual(run.call_args.args[1], "Exact prompt.")
        page.goto.assert_not_called()

    def test_saves_actual_successful_prompt_for_future_repeats(self):
        page = Mock()
        with tempfile.TemporaryDirectory() as directory, \
                patch.object(worker, "_wait_ready_or_login", return_value="ready"), \
                patch.object(worker, "_run_attempt", side_effect=[
                    ("rejected", "refused"), ("video", "/saved/video.mp4")]):
            result, _ = worker._generate_once(None, page, "Initial prompt.", [], directory, {})
            self.assertEqual(result["video"], "/saved/video.mp4")
            self.assertEqual(Path(directory, "generation_prompt.txt").read_text(),
                             worker._prompt_variant("Initial prompt.", 1))

    def test_repeat_browser_loss_is_recoverable_not_a_refusal(self):
        page = Mock()
        with tempfile.TemporaryDirectory() as directory, \
                patch.object(worker, "_run_attempt", side_effect=RuntimeError(
                    "Locator.inner_text: Target page, context or browser has been closed")), \
                patch.object(worker, "_recover_page") as recover:
            result, next_page = worker._generate_once(
                None, page, "Exact prompt.", [], directory, {"preservePrompt": True})
        self.assertTrue(result["browserLost"])
        self.assertFalse(result["rejected"])
        self.assertIsNone(next_page)
        recover.assert_not_called()

    def worker_fixture(self, outcomes, prepare=None):
        page = Mock()
        page.is_closed.return_value = False
        first_context, second_context = Mock(), Mock()
        with patch("playwright.sync_api.sync_playwright"), \
                patch.object(worker.sys, "stdin", io.StringIO('{}\n{}\n')), \
                patch.object(worker, "_read_request", return_value=(
                    "Exact prompt", "/unused", ["image.png"], {"preservePrompt": True})), \
                patch.object(worker, "_launch_context", side_effect=[
                    (first_context, "test", None), (second_context, "test", None)]) as launch, \
                patch.object(worker, "_prepare_page", side_effect=prepare,
                             return_value=(page, None)), \
                patch.object(worker, "_generate_once", side_effect=outcomes), \
                patch.object(worker, "_respond") as respond:
            self.assertEqual(worker._worker_main(), 0)
        return launch.call_count, [call.args[0] for call in respond.call_args_list]

    def test_worker_relaunches_after_browser_loss_result(self):
        page = Mock()
        page.is_closed.return_value = False
        count, responses = self.worker_fixture([
            ({"video": None, "error": "closed", "browserLost": True, "rejected": False}, None),
            ({"video": "/saved/video.mp4", "error": None, "rejected": False}, page)])
        self.assertEqual(count, 2)
        self.assertTrue(responses[0]["browserLost"])
        self.assertEqual(responses[1]["video"], "/saved/video.mp4")

    def test_worker_relaunches_after_prepare_exception(self):
        page = Mock()
        page.is_closed.return_value = False
        count, responses = self.worker_fixture([
            ({"video": "/saved/video.mp4", "error": None, "rejected": False}, page)], prepare=[
            RuntimeError("Page.goto: Target page, context or browser has been closed"),
            (page, None)])
        self.assertEqual(count, 2)
        self.assertTrue(responses[0]["browserLost"])
        self.assertEqual(responses[1]["video"], "/saved/video.mp4")

    def test_recovered_download_is_kept_and_next_job_gets_new_browser(self):
        closed_page, live_page = Mock(), Mock()
        closed_page.is_closed.return_value = True
        live_page.is_closed.return_value = False
        count, responses = self.worker_fixture([
            ({"video": "/saved/first.mp4", "error": None, "rejected": False}, closed_page),
            ({"video": "/saved/second.mp4", "error": None, "rejected": False}, live_page)])
        self.assertEqual(count, 2)
        self.assertEqual([result["video"] for result in responses],
                         ["/saved/first.mp4", "/saved/second.mp4"])


if __name__ == "__main__":
    unittest.main()
