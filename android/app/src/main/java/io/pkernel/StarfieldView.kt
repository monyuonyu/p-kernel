/*
 * StarfieldView.kt — the entry screen's living backdrop.
 *
 * A cheap, self-contained animated starfield drawn on a plain Canvas. No
 * WebView, no assets, no GL — just a couple hundred points that twinkle and
 * drift, in the galaxy.html palette (bg #05060a, stars #cdd6f4 with a few
 * #89b4fa / #f9e2af accents). This is the "ワクワク" — your phone is a star in
 * a galaxy no one owns, and the very first frame should feel like that.
 *
 * Cost: ~N points * (one drawCircle) per frame at ~display refresh via the
 * View's own invalidate loop, paused automatically when not attached /
 * not visible (onDetachedFromWindow stops posting). Deliberately frugal so
 * it never competes with the kernel for the phone's battery — and on
 * charge-only nodes the screen is usually the only thing drawing anyway.
 */
package io.pkernel

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.util.AttributeSet
import android.view.View
import kotlin.random.Random

class StarfieldView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyle: Int = 0
) : View(context, attrs, defStyle) {

    private class Star(
        var x: Float, var y: Float,
        val r: Float, val baseA: Float,
        val tw: Float, var phase: Float,
        val color: Int, val drift: Float
    )

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val stars = ArrayList<Star>()
    private var running = false

    // galaxy.html accents
    private val starWhite = Color.parseColor("#cdd6f4")
    private val starBlue  = Color.parseColor("#89b4fa")
    private val starGold  = Color.parseColor("#f9e2af")
    private val bg        = Color.parseColor("#05060a")

    private val rnd = Random(0x5EED)

    private fun seed(w: Int, h: Int) {
        stars.clear()
        if (w <= 0 || h <= 0) return
        // density scales with area but capped — frugal on huge screens.
        val n = ((w.toLong() * h) / 9000L).toInt().coerceIn(80, 240)
        repeat(n) {
            val accent = rnd.nextFloat()
            val color = when {
                accent > 0.94f -> starGold
                accent > 0.82f -> starBlue
                else           -> starWhite
            }
            stars.add(
                Star(
                    x = rnd.nextFloat() * w,
                    y = rnd.nextFloat() * h,
                    r = 0.6f + rnd.nextFloat() * 1.8f,
                    baseA = 0.25f + rnd.nextFloat() * 0.6f,
                    tw = 0.6f + rnd.nextFloat() * 1.8f,
                    phase = rnd.nextFloat() * 6.2832f,
                    color = color,
                    drift = 2f + rnd.nextFloat() * 8f   // px/sec upward
                )
            )
        }
    }

    override fun onSizeChanged(w: Int, h: Int, ow: Int, oh: Int) {
        super.onSizeChanged(w, h, ow, oh)
        seed(w, h)
    }

    private var lastT = 0L

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        canvas.drawColor(bg)

        val now = System.nanoTime()
        val dt = if (lastT == 0L) 0f else ((now - lastT) / 1_000_000_000f).coerceAtMost(0.1f)
        lastT = now

        val h = height.toFloat()
        for (s in stars) {
            // twinkle
            s.phase += s.tw * dt
            val a = (s.baseA * (0.55f + 0.45f * kotlin.math.sin(s.phase.toDouble()).toFloat()))
                .coerceIn(0f, 1f)
            // slow upward drift, wrap around
            s.y -= s.drift * dt
            if (s.y < -2f) {
                s.y = h + 2f
                s.x = rnd.nextFloat() * width
            }
            paint.color = s.color
            paint.alpha = (a * 255).toInt()
            canvas.drawCircle(s.x, s.y, s.r, paint)
        }

        if (running) postInvalidateOnAnimation()
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        running = true
        lastT = 0L
        postInvalidateOnAnimation()
    }

    override fun onDetachedFromWindow() {
        running = false
        super.onDetachedFromWindow()
    }

    override fun onWindowVisibilityChanged(visibility: Int) {
        super.onWindowVisibilityChanged(visibility)
        running = visibility == VISIBLE
        if (running) {
            lastT = 0L
            postInvalidateOnAnimation()
        }
    }
}
