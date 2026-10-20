package dev.screenbridge;

import android.content.ContentValues;
import android.content.Context;
import android.graphics.Bitmap;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.media.MediaMuxer;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.MediaStore;
import android.os.ParcelFileDescriptor;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.text.SimpleDateFormat;
import java.util.ArrayDeque;
import java.util.Date;
import java.util.Locale;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * Encodes the frames already supplied by ScreenBridge's existing MediaProjection.
 * It deliberately does not create a second VirtualDisplay or request another projection.
 */
final class ScreenBridgeRecorder {
    private static final String MIME = "video/avc";
    private static final int FRAME_INTERVAL_MS = 100; // target <= 10 fps to protect capture latency
    private static volatile Uri lastCompletedUri;

    private final Context context;
    private final int width;
    private final int height;
    private final int colorFormat;
    private final MediaCodec codec;
    private final MediaMuxer muxer;
    private final ParcelFileDescriptor outputFd;
    private final Uri outputUri;
    private final ArrayBlockingQueue<Frame> frames = new ArrayBlockingQueue<>(2);
    private final Object stateLock = new Object();
    private volatile boolean accepting = true;
    private volatile boolean failed;
    private volatile String error = "";
    private volatile boolean muxerStarted;
    private volatile boolean muxerStopped;
    private volatile int trackIndex = -1;
    private volatile long lastQueuedAtMs;
    private volatile long firstFramePtsUs = -1;
    private Thread worker;

    private static final class Frame {
        final int[] pixels;
        final long capturedAtNs;
        Frame(int[] pixels, long capturedAtNs) {
            this.pixels = pixels;
            this.capturedAtNs = capturedAtNs;
        }
    }

    static Uri lastCompletedUri() { return lastCompletedUri; }
    boolean isAccepting() { return accepting; }
    String error() { return error; }

    ScreenBridgeRecorder(Context context, int sourceWidth, int sourceHeight) throws Exception {
        this.context = context.getApplicationContext();
        float scale = Math.min(1.0f, 1280.0f / Math.max(sourceWidth, sourceHeight));
        this.width = Math.max(2, ((int)(sourceWidth * scale)) & ~1);
        this.height = Math.max(2, ((int)(sourceHeight * scale)) & ~1);

        MediaFormat format = MediaFormat.createVideoFormat(MIME, width, height);
        int selected = -1;
        MediaCodecInfo selectedInfo = null;
        for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
            if (!info.isEncoder()) continue;
            boolean supports;
            try { supports = info.getCapabilitiesForType(MIME) != null; }
            catch (Exception ignored) { supports = false; }
            if (!supports) continue;
            int[] preferred = new int[] {
                MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420SemiPlanar,
                MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Planar
            };
            for (int wanted : preferred) {
                for (int available : info.getCapabilitiesForType(MIME).colorFormats) {
                    if (available == wanted) { selected = wanted; selectedInfo = info; break; }
                }
                if (selected >= 0) break;
            }
            if (selected >= 0) break;
        }
        if (selected < 0 || selectedInfo == null) throw new IOException("No AVC encoder with supported YUV420 byte-buffer input");
        colorFormat = selected;
        format.setInteger(MediaFormat.KEY_COLOR_FORMAT, colorFormat);
        format.setInteger(MediaFormat.KEY_BIT_RATE, Math.max(700_000, width * height * 3));
        format.setInteger(MediaFormat.KEY_FRAME_RATE, 10);
        format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2);

        String stamp = new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(new Date());
        ContentValues values = new ContentValues();
        values.put(MediaStore.Video.Media.DISPLAY_NAME, "ScreenBridge_" + stamp + ".mp4");
        values.put(MediaStore.Video.Media.MIME_TYPE, "video/mp4");
        if (Build.VERSION.SDK_INT >= 29) {
            values.put(MediaStore.Video.Media.RELATIVE_PATH, Environment.DIRECTORY_MOVIES + "/ScreenBridgeTests");
            values.put(MediaStore.Video.Media.IS_PENDING, 1);
        }
        Uri inserted = this.context.getContentResolver().insert(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, values);
        if (inserted == null) throw new IOException("Could not create recording in Movies/ScreenBridgeTests");
        outputUri = inserted;
        ParcelFileDescriptor fd = null;
        MediaMuxer createdMuxer = null;
        MediaCodec createdCodec = null;
        try {
            fd = this.context.getContentResolver().openFileDescriptor(outputUri, "w");
            if (fd == null) throw new IOException("Could not open recording output");
            createdMuxer = new MediaMuxer(fd.getFileDescriptor(), MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4);
            createdCodec = MediaCodec.createByCodecName(selectedInfo.getName());
            createdCodec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            createdCodec.start();
        } catch (Exception e) {
            if (createdCodec != null) try { createdCodec.release(); } catch (Exception ignored) {}
            if (createdMuxer != null) try { createdMuxer.release(); } catch (Exception ignored) {}
            if (fd != null) try { fd.close(); } catch (Exception ignored) {}
            this.context.getContentResolver().delete(outputUri, null, null);
            throw e;
        }
        outputFd = fd;
        muxer = createdMuxer;
        codec = createdCodec;
        worker = new Thread(this::encodeLoop, "ScreenBridge-Recorder");
        worker.setDaemon(true);
        worker.start();
    }

    void submitFrame(Bitmap bitmap) {
        if (!accepting || failed || bitmap == null) return;
        long now = android.os.SystemClock.elapsedRealtime();
        if (now - lastQueuedAtMs < FRAME_INTERVAL_MS) return;
        lastQueuedAtMs = now;
        int[] pixels = new int[width * height];
        Bitmap scaled = null;
        try {
            Bitmap source = bitmap;
            if (bitmap.getWidth() != width || bitmap.getHeight() != height) {
                scaled = Bitmap.createScaledBitmap(bitmap, width, height, true);
                source = scaled;
            }
            source.getPixels(pixels, 0, width, 0, 0, width, height);
        } catch (Exception e) {
            error = "Frame copy failed: " + e.getClass().getSimpleName();
            return;
        } finally {
            if (scaled != null && scaled != bitmap) scaled.recycle();
        }
        Frame frame = new Frame(pixels, System.nanoTime());
        if (!frames.offer(frame)) {
            frames.poll();
            frames.offer(frame);
        }
    }

    void stopAndFinalize() {
        accepting = false;
        if (worker != null) {
            try {
                worker.join(7000);
                if (worker.isAlive()) {
                    failed = true;
                    error = "Recorder finalization timed out";
                    worker.interrupt();
                }
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                failed = true;
                error = "Recorder finalization interrupted";
            }
        }
    }

    private void encodeLoop() {
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        try {
            while (accepting || !frames.isEmpty()) {
                Frame frame = frames.poll(120, TimeUnit.MILLISECONDS);
                if (frame != null) encodeFrame(frame, info);
                drainEncoder(info, false);
            }
            int inputIndex;
            do {
                inputIndex = codec.dequeueInputBuffer(10_000);
            } while (inputIndex < 0 && !Thread.currentThread().isInterrupted());
            if (inputIndex >= 0) codec.queueInputBuffer(inputIndex, 0, 0, lastPresentationTimeUs() + 1, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
            drainEncoder(info, true);
        } catch (Exception e) {
            failed = true;
            error = "Recording failed: " + e.getClass().getSimpleName() + (e.getMessage() == null ? "" : " (" + e.getMessage() + ")");
        } finally {
            try { codec.stop(); } catch (Exception ignored) {}
            try { codec.release(); } catch (Exception ignored) {}
            try { if (muxerStarted && !muxerStopped) muxer.stop(); } catch (Exception ignored) { failed = true; }
            try { muxer.release(); } catch (Exception ignored) {}
            try { outputFd.close(); } catch (Exception ignored) {}
            ContentValues done = new ContentValues();
            if (Build.VERSION.SDK_INT >= 29) done.put(MediaStore.Video.Media.IS_PENDING, 0);
            if (failed) {
                context.getContentResolver().delete(outputUri, null, null);
            } else {
                context.getContentResolver().update(outputUri, done, null, null);
                lastCompletedUri = outputUri;
            }
        }
    }

    private long presentationTimeUs = 0;
    private long lastPresentationTimeUs() { return presentationTimeUs; }

    private void encodeFrame(Frame frame, MediaCodec.BufferInfo info) throws Exception {
        int index;
        do {
            index = codec.dequeueInputBuffer(10_000);
            if (index < 0) drainEncoder(info, false);
        } while (index < 0 && !Thread.currentThread().isInterrupted());
        if (index < 0) return;
        ByteBuffer input = codec.getInputBuffer(index);
        if (input == null) return;
        input.clear();
        int required = width * height * 3 / 2;
        if (input.capacity() < required) throw new IOException("Encoder input buffer is too small");
        if (firstFramePtsUs < 0) firstFramePtsUs = frame.capturedAtNs / 1000L;
        long pts = Math.max(presentationTimeUs + 1, frame.capturedAtNs / 1000L - firstFramePtsUs);
        presentationTimeUs = pts;
        convertToYuv420(frame.pixels, input, width, height, colorFormat);
        codec.queueInputBuffer(index, 0, required, pts, 0);
    }

    private void drainEncoder(MediaCodec.BufferInfo info, boolean endOfStream) throws Exception {
        int idle = 0;
        while (true) {
            int index = codec.dequeueOutputBuffer(info, endOfStream ? 10_000 : 0);
            if (index == MediaCodec.INFO_TRY_AGAIN_LATER) {
                if (!endOfStream || ++idle > 20) return;
                continue;
            }
            if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                if (muxerStarted) throw new IOException("Encoder format changed twice");
                trackIndex = muxer.addTrack(codec.getOutputFormat());
                muxer.start();
                muxerStarted = true;
                continue;
            }
            if (index < 0) continue;
            ByteBuffer output = codec.getOutputBuffer(index);
            if (output != null && info.size > 0 && muxerStarted) {
                output.position(info.offset);
                output.limit(info.offset + info.size);
                muxer.writeSampleData(trackIndex, output, info);
            }
            boolean eos = (info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
            codec.releaseOutputBuffer(index, false);
            if (eos) {
                if (muxerStarted && !muxerStopped) { muxer.stop(); muxerStopped = true; }
                return;
            }
        }
    }

    private static void convertToYuv420(int[] argb, ByteBuffer out, int width, int height, int colorFormat) {
        int frameSize = width * height;
        int yIndex = 0;
        int uvIndex = frameSize;
        int uIndex = frameSize;
        int vIndex = frameSize + frameSize / 4;
        boolean semiPlanar = colorFormat == MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420SemiPlanar;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int c = argb[y * width + x];
                int r = (c >> 16) & 255;
                int g = (c >> 8) & 255;
                int b = c & 255;
                int yy = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
                int uu = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                int vv = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                out.put(yIndex++, (byte)Math.max(0, Math.min(255, yy)));
                if ((y & 1) == 0 && (x & 1) == 0) {
                    if (semiPlanar) {
                        out.put(uvIndex++, (byte)Math.max(0, Math.min(255, uu)));
                        out.put(uvIndex++, (byte)Math.max(0, Math.min(255, vv)));
                    } else {
                        out.put(uIndex++, (byte)Math.max(0, Math.min(255, uu)));
                        out.put(vIndex++, (byte)Math.max(0, Math.min(255, vv)));
                    }
                }
            }
        }
        out.position(frameSize * 3 / 2);
    }
}
