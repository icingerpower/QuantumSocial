"""Save Gemini video blobs without invoking Chrome's native download UI.

Chrome 153.0.8010.36 crashed in DownloadBubbleUpdateService while resolving
Gemini's blob:null downloads (confirmed from the local Chrome core dump).
The capture stays scoped to video download links; normal links are untouched.
"""
import base64
import os


_CAPTURE_SCRIPT = r"""(() => {
    if (window.__qsVideoCaptureInstalled) return;
    window.__qsVideoCaptureInstalled = true;
    const blobs = new Map();
    const create = URL.createObjectURL;
    const revoke = URL.revokeObjectURL;
    URL.createObjectURL = function(value) {
        const url = create.call(this, value);
        if (value instanceof Blob) blobs.set(url, value);
        return url;
    };
    URL.revokeObjectURL = function(url) {
        blobs.delete(url);
        return revoke.call(this, url);
    };
    const videoName = name => /\.(mp4|m4v|mov|webm|mkv)$/i.test(name);
    const videoBlob = blob => blob && blob.type.startsWith('video/');
    const sendBlob = (blob, name) => {
        // Transfer ownership of a Blob reference to the top frame before
        // Gemini removes its temporary sandboxed download iframe. Reading
        // the bytes asynchronously inside that iframe would lose them.
        window.top.postMessage({qsVideoBlob: true, blob, name}, '*');
    };
    if (window === window.top) {
        window.addEventListener('message', event => {
            const data = event.data;
            if (!data || data.qsVideoBlob !== true || !(data.blob instanceof Blob)
                || (!videoName(data.name) && !videoBlob(data.blob))) return;
            const reader = new FileReader();
            reader.onload = () => window.qsSaveVideoBlob(
                data.name, data.blob.type, reader.result.split(',', 2)[1], null);
            reader.onerror = () => window.qsSaveVideoBlob(
                data.name, data.blob.type, '', 'Could not read the video blob');
            reader.readAsDataURL(data.blob);
        });
    }
    const capture = anchor => {
        if (!anchor || !anchor.hasAttribute('download')
            || !anchor.href.startsWith('blob:')) return false;
        const blob = blobs.get(anchor.href);
        const name = anchor.download || 'generated_video.mp4';
        if (!videoName(name) && !videoBlob(blob)) return false;
        if (blob) {
            sendBlob(blob, name);
        } else {
            fetch(anchor.href).then(response => response.blob())
                .then(value => sendBlob(value, name))
                .catch(error => window.qsSaveVideoBlob(name, '', '', String(error)));
        }
        return true;
    };
    const click = HTMLAnchorElement.prototype.click;
    HTMLAnchorElement.prototype.click = function() {
        if (!capture(this)) return click.call(this);
    };
    document.addEventListener('click', event => {
        const anchor = event.target.closest && event.target.closest('a[download]');
        if (capture(anchor)) {
            event.preventDefault();
            event.stopImmediatePropagation();
        }
    }, true);
})();"""


def install_video_capture(page, output_dir, log):
    """Return mutable capture state, refreshed for each generation attempt."""
    state = getattr(page, "_qs_video_capture", None)
    if state is None:
        state = {}

        def save_video(source, name, media_type, encoded, error):
            if state.get("video"):
                return  # ignore duplicate clicks after one successful capture
            try:
                if error:
                    raise ValueError(error)
                if len(encoded) > 256 * 1024 * 1024:
                    raise ValueError("Video blob exceeds the capture size limit")
                content = base64.b64decode(encoded, validate=True)
                is_mp4 = content[4:8] == b"ftyp"
                is_webm = content[:4] == b"\x1aE\xdf\xa3"
                if not (is_mp4 or is_webm):
                    raise ValueError("Download did not contain an MP4 or WebM video")
                name = os.path.basename(str(name).replace("\\", "/"))
                extensions = (".mp4", ".m4v", ".mov") if is_mp4 else (".webm", ".mkv")
                if not name.lower().endswith(extensions):
                    name = "generated_video" + extensions[0]
                target = os.path.abspath(os.path.join(state["output_dir"], name))
                temporary = target + ".part"
                try:
                    with open(temporary, "wb") as handle:
                        handle.write(content)
                    os.replace(temporary, target)
                finally:
                    if os.path.exists(temporary):
                        os.remove(temporary)
                state["video"] = target
                log(f"download: saved video blob directly ({len(content)} bytes) -> {target}")
            except Exception as exc:
                state["error"] = f"Could not save the video blob: {exc}"
                log(state["error"])

        page.expose_binding("qsSaveVideoBlob", save_video)
        page.add_init_script(_CAPTURE_SCRIPT)
        page._qs_video_capture = state
    state.update(output_dir=output_dir, video=None, error=None)
    # Also install in frames that already exist before the next navigation.
    for frame in page.frames:
        if not frame.is_detached():
            frame.evaluate(_CAPTURE_SCRIPT)
    return state
