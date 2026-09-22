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

    def test_edit_last_message_success(self):
        self.page.set_content("""
            <user-query>
                <div id="content">Original prompt</div>
                <button id="edit" aria-label="Modifier">Modifier</button>
                <textarea id="box" hidden></textarea>
                <button id="update" aria-label="Mettre à jour" hidden disabled>Mettre à jour</button>
            </user-query>
            <script>
                edit.onclick = () => {
                    box.hidden = false;
                    box.value = 'Original prompt';
                    update.hidden = false;
                };
                box.oninput = () => {
                    update.disabled = false;
                };
                update.onclick = () => {
                    window.updated = box.value;
                };
            </script>
        """)
        self.assertTrue(worker._edit_last_message(self.page, "New prompt for take 2", timeout_s=1.0))
        self.assertEqual(self.page.evaluate("window.updated"), "New prompt for take 2")

    def test_edit_last_message_returns_false_when_no_queries(self):
        self.page.set_content("<div>No queries here</div>")
        self.assertFalse(worker._edit_last_message(self.page, "Prompt", timeout_s=0.5))

    def test_edit_last_message_returns_false_when_edit_button_missing(self):
        self.page.set_content("<user-query><div>Text only</div></user-query>")
        self.assertFalse(worker._edit_last_message(self.page, "Prompt", timeout_s=0.5))

    def test_run_attempt_uses_edit_last_when_requested(self):
        self.page.set_content("<user-query><button aria-label='Modifier'></button></user-query>")
        with patch.object(worker, "_video_quota_message", return_value=None), \
                patch.object(worker, "_edit_last_message", return_value=True) as edit_mock, \
                patch.object(worker, "_select_video_tool") as select_tool, \
                patch.object(worker, "install_video_capture", return_value={"video": "/saved/take2.mp4", "error": None}):
            status, payload = worker._run_attempt(
                self.page, "Take 2 prompt", [], "/tmp", time.time() + 10, time.time() + 10,
                5, message_action="edit_last")
            self.assertEqual(status, "video")
            self.assertEqual(payload, "/saved/take2.mp4")
            edit_mock.assert_called_once_with(self.page, "Take 2 prompt")
            select_tool.assert_not_called()

    def test_prepare_page_reuses_existing_conversation(self):
        page = Mock()
        page.is_closed.return_value = False
        page.url = "https://gemini.google.com/app/12345678"
        with patch.object(worker, "_wait_ready_or_login", return_value="ready"), \
                patch.object(worker, "_ensure_ultra", return_value=None):
            result_page, error = worker._prepare_page(
                None, require_ultra=True, existing_page=page, chat_mode="conversation")
            self.assertIsNone(error)
            self.assertEqual(result_page, page)
            page.goto.assert_not_called()

    def test_handle_tool_switch_dialog_clicks_dialog_button_not_sidebar(self):
        self.page.set_content("""
            <div id="sidebar">
                <button id="sidebar_btn">Nouvelle discussion</button>
            </div>
            <div role="dialog" id="modal">
                <h2>Commencer une nouvelle discussion ?</h2>
                <p>Sélectionner cet outil lancera une nouvelle discussion.</p>
                <button role="switch" id="dont_ask">Ne plus me demander</button>
                <button id="modal_btn">Nouvelle discussion</button>
            </div>
            <script>
                window.sidebarClicked = false;
                window.modalClicked = false;
                window.dontAskClicked = false;
                sidebar_btn.onclick = () => { window.sidebarClicked = true; };
                modal_btn.onclick = () => { window.modalClicked = true; };
                dont_ask.onclick = () => { window.dontAskClicked = true; };
            </script>
        """)
        handled = worker._handle_tool_switch_dialog(self.page)
        self.assertTrue(handled)
        self.assertFalse(self.page.evaluate("window.sidebarClicked"))
        self.assertTrue(self.page.evaluate("window.modalClicked"))
        self.assertTrue(self.page.evaluate("window.dontAskClicked"))

    def test_handle_tool_switch_dialog_dismiss_does_not_click_new_chat(self):
        self.page.set_content("""
            <div role="dialog" id="modal">
                <h2>Commencer une nouvelle discussion ?</h2>
                <button aria-label="Fermer" id="close_btn">X</button>
                <button id="modal_btn">Nouvelle discussion</button>
            </div>
            <script>
                window.closeClicked = false;
                window.modalClicked = false;
                close_btn.onclick = () => { window.closeClicked = true; };
                modal_btn.onclick = () => { window.modalClicked = true; };
            </script>
        """)
        handled = worker._handle_tool_switch_dialog(self.page, action="dismiss")
        self.assertTrue(handled)
        self.assertTrue(self.page.evaluate("window.closeClicked"))
        self.assertFalse(self.page.evaluate("window.modalClicked"))

    def test_run_attempt_calls_select_tool_in_fresh_chat(self):
        self.page.set_content("<textarea aria-label='Prompt'></textarea><button aria-label='Envoyer'></button>")
        self.page.goto = Mock()
        type(self.page).url = "https://gemini.google.com/app"
        with patch.object(worker, "_video_quota_message", return_value=None), \
                patch.object(worker, "_select_video_tool", return_value=True) as select_tool, \
                patch.object(worker, "_handle_tool_switch_dialog") as switch_dialog, \
                patch.object(worker, "_attach_image", return_value=True), \
                patch.object(worker, "_submit_prompt", return_value=True), \
                patch.object(worker, "install_video_capture", return_value={"video": "/saved/step1.mp4", "error": None}):
            status, payload = worker._run_attempt(
                self.page, "Next video prompt", ["/tmp/test.png"], "/tmp",
                time.time() + 10, time.time() + 10, 5, message_action="new",
                chat_mode="fresh")
            self.assertEqual(status, "video")
            self.assertEqual(payload, "/saved/step1.mp4")
            select_tool.assert_called_once_with(self.page)
            switch_dialog.assert_called_once_with(self.page, action="confirm")

    def test_run_attempt_video_tool_disabled_treated_as_quota(self):
        self.page.set_content("<textarea aria-label='Prompt'></textarea><button aria-label='Envoyer'></button>")
        self.page.goto = Mock()
        type(self.page).url = "https://gemini.google.com/app"
        with patch.object(worker, "_video_quota_message", return_value=None), \
                patch.object(worker, "_select_video_tool", return_value="disabled") as select_tool:
            status, payload = worker._run_attempt(
                self.page, "Prompt", [], "/tmp", time.time() + 10, time.time() + 10, 5)
            self.assertEqual(status, "quota")
            self.assertIn("disabled by Google", payload)
            select_tool.assert_called_once_with(self.page)

    def test_run_attempt_in_existing_conversation_stays_in_chat(self):
        self.page.set_content("<textarea aria-label='Prompt'></textarea><button aria-label='Envoyer'></button>")
        self.page.goto = Mock()
        type(self.page).url = "https://gemini.google.com/app/77e31cb1ab773abd"
        with patch.object(worker, "_video_quota_message", return_value=None), \
                patch.object(worker, "_select_video_tool") as select_tool, \
                patch.object(worker, "_handle_tool_switch_dialog") as switch_dialog, \
                patch.object(worker, "_attach_image", return_value=True), \
                patch.object(worker, "_submit_prompt", return_value=True), \
                patch.object(worker, "install_video_capture", return_value={"video": "/saved/step2.mp4", "error": None}):
            status, payload = worker._run_attempt(
                self.page, "Next video prompt", ["/tmp/test.png"], "/tmp",
                time.time() + 10, time.time() + 10, 5, message_action="new",
                chat_mode="conversation")
            self.assertEqual(status, "video")
            self.assertEqual(payload, "/saved/step2.mp4")
            select_tool.assert_not_called()
            switch_dialog.assert_not_called()

    def test_run_attempt_does_not_click_previous_turn_download_button(self):
        # Page already has an old turn with a video and download button
        self.page.set_content("""
            <model-response>
                <video src="old.mp4"></video>
                <button aria-label="Télécharger" id="old_dl">Télécharger</button>
            </model-response>
            <textarea aria-label='Prompt'></textarea>
            <button aria-label='Envoyer'></button>
        """)
        with patch.object(worker, "_video_quota_message", return_value=None), \
                patch.object(worker, "_select_video_tool", return_value=True), \
                patch.object(worker, "_handle_tool_switch_dialog"), \
                patch.object(worker, "_submit_prompt", return_value=True), \
                patch.object(worker, "install_video_capture", return_value={"video": None, "error": None}), \
                patch.object(worker.time, "time", side_effect=[100, 100, 100, 100, 100, 120]):
            # The attempt should stall and timeout without clicking the old download button
            status, payload = worker._run_attempt(
                self.page, "New video prompt", [], "/tmp",
                105, 105, 5, message_action="new")
            self.assertEqual(status, "stalled")

    def test_run_attempt_edit_last_does_not_click_previous_turn_download_button(self):
        # Page has Turn 1 (with video) and Turn 2 (with refusal), and user edits Turn 2
        self.page.set_content("""
            <user-query id="q1"><div id="c1">Turn 1</div></user-query>
            <model-response id="r1">
                <video src="turn1.mp4"></video>
                <button aria-label="Télécharger" id="old_dl">Télécharger</button>
            </model-response>
            <user-query id="q2">
                <div id="c2">Turn 2</div>
                <button aria-label="Modifier">Modifier</button>
            </user-query>
            <model-response id="r2">
                <p>I can't make videos of real people in situations like that.</p>
            </model-response>
        """)
        with patch.object(worker, "_video_quota_message", return_value=None), \
                patch.object(worker, "_edit_last_message", return_value=True), \
                patch.object(worker, "install_video_capture", return_value={"video": None, "error": None}), \
                patch.object(worker.time, "time", side_effect=[100, 100, 100, 100, 100, 120]):
            status, payload = worker._run_attempt(
                self.page, "Take 2 prompt variant", [], "/tmp",
                105, 105, 5, message_action="edit_last", chat_mode="conversation")
            # Must NOT click Turn 1's download button or report video!
            self.assertEqual(status, "stalled")

    def test_run_attempt_detects_text_only_response_as_rejection(self):
        self.page.set_content("""
            <textarea aria-label='Prompt'></textarea>
            <button aria-label='Envoyer'></button>
        """)
        # After submit, a new model-response appears with text "Your video is ready!" but NO video
        def fake_submit(page, editor, prompt, **kwargs):
            page.set_content("""
                <model-response>
                    <p>Your video is ready!</p>
                    <button aria-label="Bonne réponse">Like</button>
                </model-response>
            """)
            return True

        with patch.object(worker, "_video_quota_message", return_value=None), \
                patch.object(worker, "_select_video_tool", return_value=True), \
                patch.object(worker, "_handle_tool_switch_dialog"), \
                patch.object(worker, "_submit_prompt", side_effect=fake_submit), \
                patch.object(worker, "install_video_capture", return_value={"video": None, "error": None}):
            status, marker = worker._run_attempt(
                self.page, "Prompt", [], "/tmp",
                time.time() + 10, time.time() + 10, 5, message_action="new")
            self.assertEqual(status, "rejected")
            self.assertIn("text without a video", marker)

    def test_run_attempt_detects_refusal_real_people_situations(self):
        self.page.set_content("""
            <textarea aria-label='Prompt'></textarea>
            <button aria-label='Envoyer'></button>
        """)
        def fake_submit(page, editor, prompt, **kwargs):
            page.set_content("""
                <model-response>
                    <p>I can't make videos of real people in situations like that. Can I help you with something else?</p>
                </model-response>
            """)
            return True

        with patch.object(worker, "_video_quota_message", return_value=None), \
                patch.object(worker, "_select_video_tool", return_value=True), \
                patch.object(worker, "_handle_tool_switch_dialog"), \
                patch.object(worker, "_submit_prompt", side_effect=fake_submit), \
                patch.object(worker, "install_video_capture", return_value={"video": None, "error": None}):
            status, marker = worker._run_attempt(
                self.page, "Prompt", [], "/tmp",
                time.time() + 10, time.time() + 10, 5, message_action="new")
            self.assertEqual(status, "rejected")
            self.assertTrue(any(p in marker for p in ("I can't make", "videos of real people", "situations like that")))

    def test_handle_tool_switch_dialog_noop_when_no_dialog(self):
        self.page.set_content("""
            <div id="sidebar">
                <button id="sidebar_btn">Nouvelle discussion</button>
            </div>
        """)
        handled = worker._handle_tool_switch_dialog(self.page)
        self.assertFalse(handled)

    def test_video_quota_in_detects_daily_limit_phrases(self):
        phrases = [
            "Sorry I can't generate more video for you today",
            "Sorry, I cannot generate more videos for you today. Try again tomorrow.",
            "You have reached your daily limit for video creation.",
            "Vous n'avez plus de vidéos pour le moment.",
            "Je ne peux plus générer de vidéo pour aujourd'hui.",
            "You have no more videos for now.",
        ]
        for phrase in phrases:
            hit = worker._video_quota_in(phrase)
            self.assertIsNotNone(hit, f"Expected quota match for: {phrase}")

    def test_run_attempt_detects_quota_exhaustion_in_response(self):
        self.page.set_content("""
            <textarea aria-label='Prompt'></textarea>
            <button aria-label='Envoyer'></button>
        """)
        def fake_submit(page, editor, prompt, **kwargs):
            page.set_content("""
                <model-response>
                    <p>Sorry I can't generate more video for you today.</p>
                </model-response>
            """)
            return True

        with patch.object(worker, "_video_quota_message", return_value=None), \
                patch.object(worker, "_select_video_tool", return_value=True), \
                patch.object(worker, "_handle_tool_switch_dialog"), \
                patch.object(worker, "_submit_prompt", side_effect=fake_submit), \
                patch.object(worker, "install_video_capture", return_value={"video": None, "error": None}):
            status, payload = worker._run_attempt(
                self.page, "Prompt", [], "/tmp",
                time.time() + 10, time.time() + 10, 5, message_action="new")
            self.assertEqual(status, "quota")
            self.assertIn("can't generate more video", payload)

    def test_generate_once_returns_quota_exceeded_dict(self):
        with patch.object(worker, "_run_attempt", return_value=("quota", "Sorry I can't generate more video for you today")):
            result, _ = worker._generate_once(
                None, self.page, "Prompt", [], "/tmp", {})
            self.assertTrue(result["quotaExceeded"])
            self.assertFalse(result["retryable"])
            self.assertFalse(result["rejected"])
            self.assertIsNone(result["video"])
            self.assertIn("Gemini daily video quota reached", result["error"])

    def test_video_quota_message_ignores_old_model_responses(self):
        self.page.set_content("""
            <model-response>
                <p>Sorry, I can't generate more videos for you today, but come back tomorrow and we can make more.</p>
            </model-response>
            <div id="composer">
                <textarea aria-label="Prompt"></textarea>
            </div>
        """)
        hit = worker._video_quota_message(self.page)
        self.assertIsNone(hit, "Old quota response in model-response must not be treated as a live quota wall")

    def test_video_quota_message_detects_banner_outside_model_responses(self):
        self.page.set_content("""
            <div class="quota-banner">
                <p>Vous n'avez plus de vidéos pour le moment. Les vidéos seront de nouveau disponibles demain.</p>
            </div>
            <div id="composer">
                <textarea aria-label="Prompt"></textarea>
            </div>
        """)
        hit = worker._video_quota_message(self.page)
        self.assertIsNotNone(hit)
        self.assertIn("n'avez plus de vidéos", hit)

    def test_run_attempt_switches_model_away_from_flash_lite(self):
        self.page.set_content("""
            <button aria-label="Flash-Lite">Flash-Lite</button>
            <textarea aria-label='Prompt'></textarea>
            <button aria-label='Envoyer'></button>
        """)
        with patch.object(worker, "_video_quota_message", return_value=None), \
                patch.object(worker, "_switch_to_video_capable_model") as switch_model, \
                patch.object(worker, "_select_video_tool", return_value=True), \
                patch.object(worker, "_handle_tool_switch_dialog"), \
                patch.object(worker, "_submit_prompt", return_value=True), \
                patch.object(worker, "install_video_capture", return_value={"video": "/path/video.mp4", "error": None}):
            status, _ = worker._run_attempt(
                self.page, "Prompt", [], "/tmp",
                time.time() + 10, time.time() + 10, 5, message_action="new",
                chat_mode="conversation")
    def test_run_attempt_with_old_quota_turn_detects_new_rejection_not_quota(self):
        self.page.set_content("""
            <model-response>
                <p>Sorry, I can't generate more videos for you today, but come back tomorrow and we can make more.</p>
            </model-response>
            <textarea aria-label='Prompt'></textarea>
            <button aria-label='Envoyer'></button>
        """)
        def fake_submit(page, editor, prompt, **kwargs):
            page.set_content("""
                <model-response>
                    <p>Sorry, I can't generate more videos for you today, but come back tomorrow and we can make more.</p>
                </model-response>
                <model-response>
                    <p>Can't generate your video. Try another prompt.</p>
                    <button aria-label="Bonne réponse">Like</button>
                </model-response>
                <textarea aria-label='Prompt'></textarea>
                <button aria-label='Envoyer'></button>
            """)
            return True

        with patch.object(worker, "_switch_to_video_capable_model"), \
                patch.object(worker, "_select_video_tool", return_value=True), \
                patch.object(worker, "_handle_tool_switch_dialog"), \
                patch.object(worker, "_submit_prompt", side_effect=fake_submit), \
                patch.object(worker, "install_video_capture", return_value={"video": None, "error": None}):
            status, payload = worker._run_attempt(
                self.page, "Prompt", [], "/tmp",
                time.time() + 10, time.time() + 10, 5, message_action="new",
                chat_mode="conversation")
            self.assertEqual(status, "rejected")
            self.assertIn("can't generate your video", payload)

    def test_continuation_retries_edit_even_when_first_action_was_new(self):
        with tempfile.TemporaryDirectory() as output, \
                patch.object(worker, "_run_attempt", side_effect=[("rejected", "refused"), ("video", "/clip.mp4")]) as attempt:
            result, _ = worker._generate_once(None, self.page, "Prompt", [], output,
                {}, chat_mode="conversation", message_action="new")
            self.assertEqual([call.kwargs["message_action"] for call in attempt.call_args_list], ["new", "edit_last"])
            self.assertEqual(result["video"], "/clip.mp4")

    def test_saved_submission_retries_edit_after_restart(self):
        with tempfile.TemporaryDirectory() as output:
            Path(output, "gemini_submission.json").write_text(worker.json.dumps({
                "promptSubmitted": True, "conversationUrl": self.page.url}))
            with patch.object(worker, "_run_attempt", return_value=("error", "download failed")) as attempt:
                result, _ = worker._generate_once(None, self.page, "Prompt", [], output,
                    {}, chat_mode="conversation", message_action="new")
            self.assertEqual(attempt.call_args.kwargs["message_action"], "edit_last")
            self.assertTrue(result["promptSubmitted"])

    def test_pre_submission_failure_does_not_edit_previous_step(self):
        with tempfile.TemporaryDirectory() as output, \
                patch.object(worker, "_run_attempt", return_value=("error", "attachment failed")) as attempt:
            result, _ = worker._generate_once(None, self.page, "Prompt", [], output,
                {}, chat_mode="conversation", message_action="new")
            self.assertEqual(attempt.call_args.kwargs["message_action"], "new")
            self.assertFalse(result["promptSubmitted"])

    def test_edit_unavailable_falls_back_to_new_submission(self):
        self.composer()
        with tempfile.TemporaryDirectory() as output, \
                patch.object(worker, "_edit_last_message", return_value=False), \
                patch.object(worker, "_video_quota_message", return_value=None), \
                patch.object(worker, "_switch_to_video_capable_model"), \
                patch.object(worker, "_select_video_tool", return_value=True), \
                patch.object(worker, "_handle_tool_switch_dialog"), \
                patch.object(worker, "_select_aspect"), \
                patch.object(worker, "_submit_prompt", return_value=True) as submit, \
                patch.object(worker, "install_video_capture", return_value={"video": "/clip.mp4", "error": None}):
            state = {}
            status, _ = worker._run_attempt(self.page, "Prompt", [], output,
                time.time() + 10, time.time() + 10, 5, message_action="edit_last", submission_state=state)
            self.assertEqual(status, "video")
            submit.assert_called_once()
            self.assertTrue(worker._saved_submission(output)["promptSubmitted"])

    def test_browser_recovery_edits_only_after_a_confirmed_submission(self):
        for submitted in (False, True):
            with self.subTest(submitted=submitted), tempfile.TemporaryDirectory() as output:
                page = Mock()
                page.url = "https://gemini.google.com/app/conversation123"
                page.is_closed.return_value = False
                recovered = Mock()
                recovered.url = page.url
                recovered.is_closed.return_value = False
                actions = []

                def attempt(*args, **kwargs):
                    actions.append(kwargs["message_action"])
                    if len(actions) == 1:
                        if submitted:
                            kwargs["submission_state"].update(promptSubmitted=True, conversationUrl=page.url)
                        raise RuntimeError("Target page has been closed")
                    return "video", "/clip.mp4"

                with patch.object(worker, "_run_attempt", side_effect=attempt), \
                        patch.object(worker, "_recover_page", return_value=recovered), \
                        patch.object(worker, "_wait_ready_or_login", return_value="ready"):
                    result, _ = worker._generate_once(None, page, "Prompt", [], output,
                        {}, chat_mode="conversation", message_action="new")
                self.assertEqual(actions, ["new", "edit_last" if submitted else "new"])
                self.assertEqual(result["video"], "/clip.mp4")
                recovered.goto.assert_called_once_with(page.url, wait_until="domcontentloaded", timeout=60000)


if __name__ == "__main__":
    unittest.main()
