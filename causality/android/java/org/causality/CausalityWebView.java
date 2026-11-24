// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

package org.causality;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.Outline;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.os.Message;
import android.util.Log;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewOutlineProvider;
import android.webkit.JavascriptInterface;
import android.webkit.JsPromptResult;
import android.webkit.JsResult;
import android.webkit.RenderProcessGoneDetail;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.EditText;
import android.widget.FrameLayout;

import org.json.JSONObject;

import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.Locale;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Companion of Causality's native Android web view backend (webview_android.c).
 *
 * Owns one android.webkit.WebView inside a clipping FrameLayout that is added above the render
 * surface in the host window's FrameLayout. Every public instance method may be called from any
 * thread and is marshalled to the Android main looper. Events travel to native code through the
 * static native methods below, keyed by an opaque handle that native code validates.
 *
 * Assumes minSdk 24 and compileSdk 26 or newer. Document-start injection is emulated: the bridge
 * and the init script are evaluated from onPageStarted, onPageCommitVisible and onPageFinished,
 * guarded so they run once per document. Scripts of the page itself can therefore run before them.
 * Without androidx.webkit the init script runs in the main frame only.
 */
public final class CausalityWebView {
    private static final String TAG = "CausalityWebView";
    private static final String BRIDGE_NAME = "__causalityBridge";
    private static final long CREATE_TIMEOUT_MS = 5000;
    private static final long PROBE_TIMEOUT_MS = 10000;
    private static final int REQUEST_FILE_CHOOSER = 0x4357;
    private static final float EPSILON = 0.01f;

    /** Event kinds sent through nativeEvent; the order is mirrored by webview_android.c. */
    private static final int EVENT_LOAD_STARTED = 0;
    private static final int EVENT_LOAD_FINISHED = 1;
    private static final int EVENT_LOAD_FAILED = 2;
    private static final int EVENT_URL_CHANGED = 3;
    private static final int EVENT_TITLE_CHANGED = 4;
    private static final int EVENT_PROGRESS_CHANGED = 5;
    private static final int EVENT_HISTORY_CHANGED = 6;
    private static final int EVENT_NEW_WINDOW = 7;
    private static final int EVENT_PROCESS_TERMINATED = 8;

    private static final Handler MAIN = new Handler(Looper.getMainLooper());

    private static ValueCallback<Uri[]> pendingChooser;
    private static CausalityWebView chooserOwner;

    private final long handle;
    private final FrameLayout host;
    private final FrameLayout clip;
    private final String userAgent;
    private final String nonce;
    private final String bridgeScript;
    private final boolean transparent;
    private final ArrayList<Dialog> dialogs = new ArrayList<>();

    private volatile boolean alive = true;
    private volatile boolean focused;

    private PageView page;
    private String url = "";
    private String title = "";
    private int progress;
    private boolean loading;
    private boolean failed;
    private boolean canGoBack;
    private boolean canGoForward;
    private double zoom = 1.0;
    private int cornerRadiusPx;
    private int pageLeft;
    private int pageTop;
    private int pageWidth;
    private int pageHeight;

    private static native void nativeEvent(long handle, int kind, String url, String title,
                                           int progress, boolean loading, boolean canGoBack,
                                           boolean canGoForward, String detail);

    private static native void nativeMessage(long handle, String message);

    private static native void nativeEvalResult(long handle, long id, boolean ok, String text);

    private static native boolean nativeNavigate(long handle, String url);

    private static native void nativeKey(long handle, int pressed);

    /**
     * Create a web view on the main thread, waiting for it when called from another thread.
     *
     * @param handle      Opaque native handle echoed in every native callback.
     * @param host        Window FrameLayout the web view is added to.
     * @param userAgent   Custom user agent, or null for the default.
     * @param initScript  Script evaluated once per main-frame document after the bridge, or null.
     * @param transparent Whether the page background lets the render surface show through.
     * @param devtools    Whether remote debugging (chrome://inspect) is enabled for the process.
     * @return The web view, or null when it could not be created.
     */
    public static CausalityWebView create(long handle, ViewGroup host, String userAgent,
                                          String initScript, boolean transparent,
                                          boolean devtools) {
        if (!(host instanceof FrameLayout)) return null;
        final FrameLayout frame = (FrameLayout) host;
        if (Looper.myLooper() == Looper.getMainLooper())
            return build(handle, frame, userAgent, initScript, transparent, devtools);

        final Object lock = new Object();
        final AtomicReference<CausalityWebView> result = new AtomicReference<>();
        final boolean[] abandoned = {false};
        final CountDownLatch done = new CountDownLatch(1);
        MAIN.post(() -> {
            synchronized (lock) {
                if (!abandoned[0])
                    result.set(build(handle, frame, userAgent, initScript, transparent, devtools));
            }
            done.countDown();
        });
        boolean finished;
        try {
            finished = done.await(CREATE_TIMEOUT_MS, TimeUnit.MILLISECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            finished = false;
        }
        synchronized (lock) {
            if (!finished) abandoned[0] = true;
            return result.get();
        }
    }

    /**
     * Construct a web view, converting platform failures into null.
     *
     * @param handle      Native handle.
     * @param host        Window FrameLayout.
     * @param userAgent   Custom user agent or null.
     * @param initScript  Init script or null.
     * @param transparent Transparent page background.
     * @param devtools    Enable remote debugging.
     * @return The web view or null.
     */
    private static CausalityWebView build(long handle, FrameLayout host, String userAgent,
                                          String initScript, boolean transparent,
                                          boolean devtools) {
        try {
            return new CausalityWebView(handle, host, userAgent, initScript, transparent, devtools);
        } catch (RuntimeException e) {
            Log.e(TAG, "web view creation failed", e);
            return null;
        }
    }

    /**
     * Build the clip container and page and attach them above the host's render surface.
     * Must run on the main thread.
     *
     * @param handle      Native handle.
     * @param host        Window FrameLayout.
     * @param userAgent   Custom user agent or null.
     * @param initScript  Init script or null.
     * @param transparent Transparent page background.
     * @param devtools    Enable remote debugging.
     */
    private CausalityWebView(long handle, FrameLayout host, String userAgent, String initScript,
                             boolean transparent, boolean devtools) {
        this.handle = handle;
        this.host = host;
        this.userAgent = userAgent;
        this.transparent = transparent;
        this.nonce = newNonce();
        this.bridgeScript = buildBridgeScript(nonce, initScript);
        if (devtools) WebView.setWebContentsDebuggingEnabled(true);

        clip = new FrameLayout(host.getContext());
        clip.setClipChildren(true);
        clip.setClipToPadding(true);
        clip.setVisibility(View.GONE);
        clip.setOutlineProvider(new ViewOutlineProvider() {
            @Override
            public void getOutline(View view, Outline outline) {
                outline.setRoundRect(0, 0, view.getWidth(), view.getHeight(), cornerRadiusPx);
            }
        });
        page = buildPage();
        attachPage();
        host.addView(clip, new FrameLayout.LayoutParams(0, 0, Gravity.TOP | Gravity.START));
        if (!(clip.getLayoutParams() instanceof FrameLayout.LayoutParams)) {
            host.removeView(clip);
            clip.removeAllViews();
            page.destroy();
            throw new IllegalArgumentException("host view does not use FrameLayout.LayoutParams");
        }
    }

    /**
     * Generate the secret that authenticates bridge messages from the main frame.
     *
     * @return 32 hexadecimal characters.
     */
    private static String newNonce() {
        byte[] bytes = new byte[16];
        new SecureRandom().nextBytes(bytes);
        StringBuilder text = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) text.append(String.format(Locale.ROOT, "%02x", b));
        return text.toString();
    }

    /**
     * Compose the script that installs the frozen window.causality bridge and runs the init script.
     * It does nothing in sub-frames or when window.causality already exists.
     *
     * @param nonce      Secret the bridge presents with every message.
     * @param initScript User script run after the bridge, or null.
     * @return JavaScript source.
     */
    private static String buildBridgeScript(String nonce, String initScript) {
        StringBuilder source = new StringBuilder(512);
        source.append("(function(){if(window.top!==window||window.causality)return;")
              .append("var b=window.").append(BRIDGE_NAME).append(";if(!b)return;")
              .append("var n='").append(nonce).append("';")
              .append("Object.defineProperty(window,'causality',{value:Object.freeze({")
              .append("postMessage:function(m){var s=typeof m==='string'?m:JSON.stringify(m);")
              .append("if(typeof s==='string')b.postMessage(n,s);}}),configurable:false});");
        if (initScript != null && !initScript.isEmpty())
            source.append("try{(0,eval)(").append(JSONObject.quote(initScript))
                  .append(");}catch(e){console.error(e);}");
        source.append("})();");
        return source.toString();
    }

    /**
     * Create and configure a page. Must run on the main thread.
     *
     * @return A new, unattached page.
     */
    private PageView buildPage() {
        PageView view = new PageView(host.getContext(), this);
        WebSettings settings = view.getSettings();
        settings.setJavaScriptEnabled(true);
        settings.setDomStorageEnabled(true);
        settings.setAllowFileAccess(true);
        settings.setSupportMultipleWindows(true);
        settings.setJavaScriptCanOpenWindowsAutomatically(true);
        if (userAgent != null) settings.setUserAgentString(userAgent);
        if (transparent) view.setBackgroundColor(Color.TRANSPARENT);
        view.addJavascriptInterface(new Bridge(), BRIDGE_NAME);
        view.setWebViewClient(new Client());
        view.setWebChromeClient(new Chrome());
        return view;
    }

    /** Add the current page to the clip container using the stored page layout. */
    private void attachPage() {
        FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(
                pageWidth, pageHeight, Gravity.TOP | Gravity.START);
        params.leftMargin = pageLeft;
        params.topMargin = pageTop;
        clip.addView(page, params);
    }

    /**
     * Run a task on the main looper, inline when already there.
     *
     * @param task Work to run.
     */
    private static void runOnMain(Runnable task) {
        if (Looper.myLooper() == Looper.getMainLooper()) task.run();
        else MAIN.post(task);
    }

    /**
     * Report an event with the current state snapshot to native code.
     *
     * @param kind   EVENT_* constant.
     * @param detail Message, error text or new-window url; may be null.
     */
    private void emit(int kind, String detail) {
        if (!alive) return;
        nativeEvent(handle, kind, url, title, progress, loading, canGoBack, canGoForward, detail);
    }

    /**
     * Record a new main-frame address and report it when it changed.
     *
     * @param next New address; null reads as empty.
     */
    private void updateUrl(String next) {
        String value = next == null ? "" : next;
        if (value.equals(url)) return;
        url = value;
        emit(EVENT_URL_CHANGED, null);
    }

    /** Re-read the history flags from the page and report a change. */
    private void syncHistory() {
        boolean back = page.canGoBack();
        boolean forward = page.canGoForward();
        if (back == canGoBack && forward == canGoForward) return;
        canGoBack = back;
        canGoForward = forward;
        emit(EVENT_HISTORY_CHANGED, null);
    }

    /**
     * Build the script applying the current page zoom to the document element.
     *
     * @return JavaScript source.
     */
    private String zoomScript() {
        String value = zoom == 1.0 ? "" : String.format(Locale.ROOT, "%.4f", zoom);
        return "(function(){var r=document.documentElement;if(r)r.style.zoom='" + value + "';})();";
    }

    /** Evaluate the bridge, init and zoom scripts in the current document. */
    private void injectDocumentScripts() {
        page.evaluateJavascript(bridgeScript + zoomScript(), null);
    }

    /**
     * Return focus to the host view when the page holds it.
     */
    private void releaseFocus() {
        if (!focused && !page.hasFocus()) return;
        page.clearFocus();
        focused = false;
        host.requestFocus();
    }

    /**
     * Intercept F9 so the host window can toggle its debug overlay.
     *
     * @param event Key event dispatched to the page.
     * @return true when the event was consumed.
     */
    private boolean interceptKey(KeyEvent event) {
        if (event.getKeyCode() != KeyEvent.KEYCODE_F9) return false;
        if (!alive) return true;
        int action = event.getAction();
        if (action == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0) nativeKey(handle, 1);
        else if (action == KeyEvent.ACTION_UP) nativeKey(handle, 0);
        return true;
    }

    /**
     * Replace a page whose render process died; history and content are lost.
     */
    private void recoverPage() {
        PageView dead = page;
        clip.removeView(dead);
        page = buildPage();
        attachPage();
        url = "";
        title = "";
        progress = 0;
        loading = false;
        failed = false;
        canGoBack = false;
        canGoForward = false;
        focused = false;
        MAIN.post(dead::destroy);
        emit(EVENT_PROCESS_TERMINATED, null);
    }

    /**
     * Place and show or hide the view.
     *
     * @param x            Page left in points.
     * @param y            Page top in points.
     * @param w            Page width in points.
     * @param h            Page height in points.
     * @param clipX        Visible left in points.
     * @param clipY        Visible top in points.
     * @param clipW        Visible width in points.
     * @param clipH        Visible height in points.
     * @param cornerRadius Corner radius in points, applied only when nothing is clipped.
     * @param visible      false hides the view.
     * @param density      Device pixels per point.
     */
    public void setFrame(float x, float y, float w, float h, float clipX, float clipY,
                         float clipW, float clipH, float cornerRadius, boolean visible,
                         float density) {
        runOnMain(() -> {
            if (!alive) return;
            if (!visible) {
                releaseFocus();
                clip.setVisibility(View.GONE);
                return;
            }
            int clipLeft = Math.round(clipX * density);
            int clipTop = Math.round(clipY * density);
            int clipRight = Math.round((clipX + clipW) * density);
            int clipBottom = Math.round((clipY + clipH) * density);
            int left = Math.round(x * density);
            int top = Math.round(y * density);
            boolean unclipped = Math.abs(clipX - x) < EPSILON && Math.abs(clipY - y) < EPSILON
                    && Math.abs(clipW - w) < EPSILON && Math.abs(clipH - h) < EPSILON;
            cornerRadiusPx = unclipped ? Math.max(0, Math.round(cornerRadius * density)) : 0;
            clip.setClipToOutline(cornerRadiusPx > 0);
            clip.invalidateOutline();

            FrameLayout.LayoutParams clipParams = (FrameLayout.LayoutParams) clip.getLayoutParams();
            clipParams.leftMargin = clipLeft;
            clipParams.topMargin = clipTop;
            clipParams.width = Math.max(0, clipRight - clipLeft);
            clipParams.height = Math.max(0, clipBottom - clipTop);
            clip.setLayoutParams(clipParams);

            pageLeft = left - clipLeft;
            pageTop = top - clipTop;
            pageWidth = Math.max(0, Math.round((x + w) * density) - left);
            pageHeight = Math.max(0, Math.round((y + h) * density) - top);
            FrameLayout.LayoutParams pageParams = (FrameLayout.LayoutParams) page.getLayoutParams();
            pageParams.leftMargin = pageLeft;
            pageParams.topMargin = pageTop;
            pageParams.width = pageWidth;
            pageParams.height = pageHeight;
            page.setLayoutParams(pageParams);
            clip.setVisibility(View.VISIBLE);
        });
    }

    /**
     * Navigate to an absolute URL.
     *
     * @param target URL to load.
     */
    public void loadUrl(String target) {
        runOnMain(() -> {
            if (alive) page.loadUrl(target);
        });
    }

    /**
     * Load an HTML document.
     *
     * @param html    Document source.
     * @param baseUrl Base address for relative links, or null.
     */
    public void loadHtml(String html, String baseUrl) {
        runOnMain(() -> {
            if (alive) page.loadDataWithBaseURL(baseUrl, html, "text/html", "UTF-8", null);
        });
    }

    /** Reload the current page. */
    public void reload() {
        runOnMain(() -> {
            if (alive) page.reload();
        });
    }

    /** Stop a load in progress and publish the resulting state. */
    public void stop() {
        runOnMain(() -> {
            if (!alive) return;
            page.stopLoading();
            if (loading) {
                loading = false;
                emit(EVENT_PROGRESS_CHANGED, null);
            }
        });
    }

    /** Move back in history when possible. */
    public void goBack() {
        runOnMain(() -> {
            if (alive && page.canGoBack()) page.goBack();
        });
    }

    /** Move forward in history when possible. */
    public void goForward() {
        runOnMain(() -> {
            if (alive && page.canGoForward()) page.goForward();
        });
    }

    /**
     * Set the page zoom through the CSS zoom property of the document element, which, unlike
     * WebSettings.setTextZoom, scales images and layout too. It is reapplied on every document.
     *
     * @param factor Zoom factor; non-positive or non-finite values are ignored.
     */
    public void setZoom(double factor) {
        if (!(factor > 0.0) || Double.isInfinite(factor)) return;
        runOnMain(() -> {
            if (!alive) return;
            zoom = factor;
            page.evaluateJavascript(zoomScript(), null);
        });
    }

    /**
     * Run a script in the main frame. The completion value arrives through nativeEvalResult as JSON
     * ("null" for null and undefined). WebView does not expose script errors, so a thrown
     * exception also completes with "null".
     *
     * @param script Source text.
     * @param id     Completion token, or 0 for no completion.
     */
    public void eval(String script, long id) {
        runOnMain(() -> {
            if (!alive) return;
            ValueCallback<String> callback = id == 0 ? null : value -> {
                if (alive) nativeEvalResult(handle, id, true, value == null ? "null" : value);
            };
            try {
                page.evaluateJavascript(script, callback);
            } catch (RuntimeException e) {
                if (id != 0) nativeEvalResult(handle, id, false, String.valueOf(e.getMessage()));
            }
        });
    }

    /** Give the page keyboard focus. */
    public void focus() {
        runOnMain(() -> {
            if (alive && clip.getVisibility() == View.VISIBLE) page.requestFocus();
        });
    }

    /** Return keyboard focus to the host view. */
    public void blur() {
        runOnMain(() -> {
            if (alive) releaseFocus();
        });
    }

    /**
     * Report whether the page holds keyboard focus; safe from any thread.
     *
     * @return true when focused.
     */
    public boolean hasFocus() {
        return focused;
    }

    /**
     * Detach and release the web view. No native callback is made after this returns.
     */
    public void destroy() {
        alive = false;
        runOnMain(() -> {
            for (Dialog dialog : new ArrayList<>(dialogs)) dialog.cancel();
            if (chooserOwner == this) cancelFileChooser();
            releaseFocus();
            page.stopLoading();
            page.removeJavascriptInterface(BRIDGE_NAME);
            page.setWebViewClient(new WebViewClient());
            page.setWebChromeClient(null);
            clip.removeAllViews();
            host.removeView(clip);
            page.destroy();
        });
    }

    /**
     * Deliver the result of the file chooser started by a page; the host activity forwards its
     * onActivityResult here.
     *
     * @param requestCode Request code of the result.
     * @param resultCode  Activity result code.
     * @param data        Result intent.
     * @return true when the result belonged to the file chooser.
     */
    public static boolean handleActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != REQUEST_FILE_CHOOSER) return false;
        ValueCallback<Uri[]> callback = pendingChooser;
        pendingChooser = null;
        chooserOwner = null;
        if (callback != null)
            callback.onReceiveValue(WebChromeClient.FileChooserParams.parseResult(resultCode, data));
        return true;
    }

    /** Resolve a pending file chooser as cancelled. */
    private static void cancelFileChooser() {
        ValueCallback<Uri[]> callback = pendingChooser;
        pendingChooser = null;
        chooserOwner = null;
        if (callback != null) callback.onReceiveValue(null);
    }

    /**
     * Find the live activity behind a context.
     *
     * @param context Context to unwrap.
     * @return The activity, or null when there is none or it is finishing.
     */
    private static Activity activityOf(Context context) {
        while (context instanceof ContextWrapper) {
            if (context instanceof Activity) {
                Activity activity = (Activity) context;
                return activity.isFinishing() || activity.isDestroyed() ? null : activity;
            }
            context = ((ContextWrapper) context).getBaseContext();
        }
        return null;
    }

    /**
     * Show a dialog for a pending JavaScript request, cancelling the request when it cannot show.
     *
     * @param dialog Dialog to show; its buttons resolve the request.
     * @param result Request to cancel on failure.
     */
    private void present(AlertDialog dialog, JsResult result) {
        dialogs.add(dialog);
        dialog.setOnDismissListener(d -> dialogs.remove(d));
        try {
            dialog.show();
        } catch (RuntimeException e) {
            dialogs.remove(dialog);
            result.cancel();
        }
    }

    /** Page that forwards the host-reserved F9 key and tracks keyboard focus. */
    private static final class PageView extends WebView {
        private final CausalityWebView owner;

        /**
         * @param context Context the page is created in.
         * @param owner   Web view that owns this page.
         */
        PageView(Context context, CausalityWebView owner) {
            super(context);
            this.owner = owner;
        }

        @Override
        public boolean dispatchKeyEvent(KeyEvent event) {
            return owner.interceptKey(event) || super.dispatchKeyEvent(event);
        }

        @Override
        protected void onFocusChanged(boolean gainFocus, int direction,
                                      android.graphics.Rect previouslyFocusedRect) {
            super.onFocusChanged(gainFocus, direction, previouslyFocusedRect);
            if (owner.page == this) owner.focused = gainFocus;
        }
    }

    /** JavaScript interface exposed to pages as __causalityBridge. */
    public final class Bridge {
        /**
         * Receive a message from the main frame; messages without the right nonce, which is only
         * known to the injected script, are dropped so sub-frames cannot spoof the host.
         *
         * @param candidate Secret presented by the caller.
         * @param message   Message text.
         */
        @JavascriptInterface
        public void postMessage(String candidate, String message) {
            if (alive && message != null && nonce.equals(candidate)) nativeMessage(handle, message);
        }
    }

    /** Navigation and load lifecycle observer. */
    private final class Client extends WebViewClient {
        @Override
        public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
            if (view != page || !alive || !request.isForMainFrame()) return false;
            return !nativeNavigate(handle, request.getUrl().toString());
        }

        @Override
        public void onPageStarted(WebView view, String pageUrl, Bitmap favicon) {
            if (view != page || !alive) return;
            failed = false;
            loading = true;
            progress = 0;
            injectDocumentScripts();
            emit(EVENT_LOAD_STARTED, null);
            updateUrl(pageUrl);
            syncHistory();
        }

        @Override
        public void onPageCommitVisible(WebView view, String pageUrl) {
            if (view != page || !alive) return;
            injectDocumentScripts();
            updateUrl(pageUrl);
        }

        @Override
        public void onPageFinished(WebView view, String pageUrl) {
            if (view != page || !alive) return;
            loading = false;
            progress = 100;
            injectDocumentScripts();
            updateUrl(pageUrl);
            syncHistory();
            if (!failed) emit(EVENT_LOAD_FINISHED, null);
        }

        @Override
        public void doUpdateVisitedHistory(WebView view, String visited, boolean isReload) {
            if (view != page || !alive) return;
            updateUrl(visited);
            syncHistory();
        }

        @Override
        public void onReceivedError(WebView view, WebResourceRequest request,
                                    WebResourceError error) {
            if (view != page || !alive || !request.isForMainFrame()) return;
            String description = String.valueOf(error.getDescription());
            if (description.contains("ERR_ABORTED")) return;
            failed = true;
            loading = false;
            emit(EVENT_LOAD_FAILED, description);
        }

        @Override
        public boolean onRenderProcessGone(WebView view, RenderProcessGoneDetail detail) {
            if (view == page && alive) recoverPage();
            return true;
        }
    }

    /** Reports the address a page wanted to open in a new window, then discards itself. */
    private final class NewWindowProbe extends WebViewClient {
        private final WebView probe;
        private boolean reported;
        private boolean released;

        /** @param probe Throw-away web view that receives the new window's navigation. */
        NewWindowProbe(WebView probe) {
            this.probe = probe;
        }

        /** Destroy the probe once. */
        void release() {
            if (released) return;
            released = true;
            probe.destroy();
        }

        /**
         * @param target Address the new window navigates to.
         */
        private void report(String target) {
            if (reported || target == null || target.isEmpty() || target.equals("about:blank"))
                return;
            reported = true;
            if (alive) emit(EVENT_NEW_WINDOW, target);
            MAIN.post(this::release);
        }

        @Override
        public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
            report(request.getUrl().toString());
            return true;
        }

        @Override
        public void onPageStarted(WebView view, String pageUrl, Bitmap favicon) {
            report(pageUrl);
        }
    }

    /** Title, progress, dialog, new-window and file chooser handling. */
    private final class Chrome extends WebChromeClient {
        @Override
        public void onProgressChanged(WebView view, int newProgress) {
            if (view != page || !alive) return;
            progress = Math.max(0, Math.min(100, newProgress));
            emit(EVENT_PROGRESS_CHANGED, null);
        }

        @Override
        public void onReceivedTitle(WebView view, String received) {
            if (view != page || !alive) return;
            String value = received == null ? "" : received;
            if (value.equals(title)) return;
            title = value;
            emit(EVENT_TITLE_CHANGED, null);
        }

        @Override
        public boolean onCreateWindow(WebView view, boolean isDialog, boolean isUserGesture,
                                      Message resultMsg) {
            if (view != page || !alive || !(resultMsg.obj instanceof WebView.WebViewTransport))
                return false;
            WebView probe = new WebView(view.getContext());
            NewWindowProbe client = new NewWindowProbe(probe);
            probe.setWebViewClient(client);
            ((WebView.WebViewTransport) resultMsg.obj).setWebView(probe);
            resultMsg.sendToTarget();
            MAIN.postDelayed(client::release, PROBE_TIMEOUT_MS);
            return true;
        }

        @Override
        public boolean onJsAlert(WebView view, String origin, String message, JsResult result) {
            Activity activity = activityOf(view.getContext());
            if (view != page || !alive || activity == null) {
                result.cancel();
                return true;
            }
            present(new AlertDialog.Builder(activity)
                    .setMessage(message)
                    .setPositiveButton(android.R.string.ok, (d, w) -> result.confirm())
                    .setOnCancelListener(d -> result.cancel())
                    .create(), result);
            return true;
        }

        @Override
        public boolean onJsConfirm(WebView view, String origin, String message, JsResult result) {
            Activity activity = activityOf(view.getContext());
            if (view != page || !alive || activity == null) {
                result.cancel();
                return true;
            }
            present(new AlertDialog.Builder(activity)
                    .setMessage(message)
                    .setPositiveButton(android.R.string.ok, (d, w) -> result.confirm())
                    .setNegativeButton(android.R.string.cancel, (d, w) -> result.cancel())
                    .setOnCancelListener(d -> result.cancel())
                    .create(), result);
            return true;
        }

        @Override
        public boolean onJsPrompt(WebView view, String origin, String message,
                                  String defaultValue, JsPromptResult result) {
            Activity activity = activityOf(view.getContext());
            if (view != page || !alive || activity == null) {
                result.cancel();
                return true;
            }
            EditText input = new EditText(activity);
            input.setSingleLine(true);
            input.setText(defaultValue == null ? "" : defaultValue);
            present(new AlertDialog.Builder(activity)
                    .setMessage(message)
                    .setView(input)
                    .setPositiveButton(android.R.string.ok,
                            (d, w) -> result.confirm(input.getText().toString()))
                    .setNegativeButton(android.R.string.cancel, (d, w) -> result.cancel())
                    .setOnCancelListener(d -> result.cancel())
                    .create(), result);
            return true;
        }

        @Override
        public boolean onShowFileChooser(WebView view, ValueCallback<Uri[]> callback,
                                         FileChooserParams params) {
            cancelFileChooser();
            Activity activity = activityOf(view.getContext());
            if (view != page || !alive || activity == null) {
                callback.onReceiveValue(null);
                return true;
            }
            pendingChooser = callback;
            chooserOwner = CausalityWebView.this;
            try {
                activity.startActivityForResult(params.createIntent(), REQUEST_FILE_CHOOSER);
            } catch (ActivityNotFoundException | SecurityException e) {
                cancelFileChooser();
            }
            return true;
        }
    }
}
