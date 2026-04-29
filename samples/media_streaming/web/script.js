/*
 * Tiny client-side controller for the http-c media-streaming demo.
 *
 * Both <video> and <audio> use the loop attribute, so playback restarts
 * automatically until the user clicks "Stop everything". We set the src
 * lazily on the first play to avoid the browser pre-fetching media that
 * the user may not actually want.
 */
(function () {
    "use strict";

    const VIDEO_SRC = "/media/sample.mp4";
    const AUDIO_SRC = "/media/sample.mp3";

    const video = document.getElementById("player-video");
    const audio = document.getElementById("player-audio");
    const btnPlayVideo = document.getElementById("btn-video-play");
    const btnPlayAudio = document.getElementById("btn-audio-play");
    const btnStop      = document.getElementById("btn-stop");
    const status       = document.getElementById("status");

    function setStatus(msg) {
        status.textContent = msg;
    }

    function ensureSrc(el, src) {
        if (!el.getAttribute("src")) {
            el.setAttribute("src", src);
        }
    }

    function playOnly(target, other, label) {
        other.pause();
        ensureSrc(target, target === video ? VIDEO_SRC : AUDIO_SRC);
        target.currentTime = 0;
        target.loop = true;
        target.play().then(
            () => setStatus("playing " + label + " (looping)"),
            (err) => setStatus("play failed: " + err.message)
        );
    }

    btnPlayVideo.addEventListener("click", () => playOnly(video, audio, "video"));
    btnPlayAudio.addEventListener("click", () => playOnly(audio, video, "audio"));

    btnStop.addEventListener("click", () => {
        video.pause();
        audio.pause();
        video.currentTime = 0;
        audio.currentTime = 0;
        setStatus("stopped");
    });

    /* Surface playback errors (e.g. cert/CORS issues) to the user. */
    [video, audio].forEach((el) => {
        el.addEventListener("error", () => {
            const code = el.error ? el.error.code : "?";
            setStatus("media error (code " + code + ")");
        });
    });

    setStatus("idle");
})();
