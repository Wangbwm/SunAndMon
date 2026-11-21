package com.limelight.ui

import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.Drawable
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.RippleDrawable
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.accessibility.AccessibilityNodeInfo
import android.widget.Button
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.core.content.ContextCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import com.limelight.R
import com.limelight.computers.CenterDirectory
import com.limelight.utils.AppTheme
import kotlin.math.roundToInt

internal class CenterDeviceView(
    context: Context,
    private val refresh: () -> Unit,
    private val centerSettings: () -> Unit,
    private val localNetwork: () -> Unit,
    private val streamSettings: () -> Unit,
    private val deviceAction: (CenterDirectory.Device, Action) -> Unit,
) : FrameLayout(context) {
    enum class Action { DESKTOP, APPS, SECONDARY_SCREEN, DEVICE_OPTIONS }

    private val palette = AppTheme.configurationContext(context)
    private val ink = color(R.color.center_ink)
    private val muted = color(R.color.center_muted)
    private val line = color(R.color.center_line)
    private val accent = color(R.color.center_accent)
    private val sky = color(R.color.center_selected)
    private val paper = color(R.color.center_canvas)
    private val surface = color(R.color.center_surface)
    private val green = color(R.color.center_online_ink)
    private val greenFill = color(R.color.center_online_fill)
    private val onAccent = color(R.color.center_on_accent)
    private val rippleColor = ColorStateList.valueOf(color(R.color.center_ripple))
    private val root = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
    private var devices = emptyList<CenterDirectory.Device>()
    private var connected = false
    private var selectedId: String? = null
    private var loading = false

    init {
        setBackgroundColor(paper)
        addView(root, LayoutParams(-1, -1))
        ViewCompat.setOnApplyWindowInsetsListener(this) { view, insets ->
            val safe = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            view.setPadding(safe.left, safe.top, safe.right, safe.bottom)
            insets
        }
    }

    fun update(devices: List<CenterDirectory.Device>, connected: Boolean, loading: Boolean = false) {
        this.devices = devices
        this.connected = connected
        this.loading = loading
        render()
    }

    fun backToDevices(): Boolean {
        if (selectedId == null) return false
        selectedId = null
        render()
        return true
    }

    private fun render() {
        root.removeAllViews()
        val content = column().apply { setPadding(dp(20), dp(12), dp(20), dp(18)) }
        val scroll = ScrollView(context).apply {
            isFillViewport = true
            clipToPadding = false
            addView(content, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        }
        root.addView(scroll, LinearLayout.LayoutParams(-1, 0, 1f))
        val selected = devices.firstOrNull { it.id == selectedId }
        if (selected == null) {
            selectedId = null
            showDevices(content)
        } else {
            showDetails(content, selected)
        }
    }

    private fun showDevices(content: LinearLayout) {
        val header = row()
        header.addView(label(R.string.center_my_devices, 26, bold = true), LinearLayout.LayoutParams(0, -2, 1f))
        header.addView(button(R.string.center_refresh, enabled = !loading, listener = refresh))
        content.addView(header)
        gap(content, 14)

        val center = row().apply {
            setPadding(dp(12), 0, dp(12), 0)
            action(sky, stroke = null, listener = centerSettings)
        }
        center.addView(icon(R.drawable.ic_center_server))
        center.addView(label(R.string.center_server, 15), LinearLayout.LayoutParams(0, -2, 1f))
        center.addView(if (connected) status(R.string.center_connected, online = true)
            else label(R.string.center_sign_in, 13, color = accent))
        center.addView(chevron())
        content.addView(center, LinearLayout.LayoutParams(-1, dp(52)))
        gap(content, 22)

        if (devices.isEmpty()) {
            val emptyText = when {
                loading -> R.string.center_loading
                connected -> R.string.center_empty
                else -> R.string.center_sign_in_hint
            }
            content.addView(label(emptyText, 14, color = muted))
        } else {
            val caption = row()
            caption.addView(label(R.string.center_all_devices, 13, bold = true), LinearLayout.LayoutParams(0, -2, 1f))
            val online = devices.count { it.online }
            caption.addView(label(0, 13, color = muted).apply {
                text = resources.getQuantityString(R.plurals.center_online_count, online, online)
            })
            content.addView(caption)
            gap(content, 10)
            devices.forEach { device ->
                val tile = row().apply {
                    setPadding(dp(12), 0, dp(12), 0)
                    action(surface) {
                        selectedId = device.id
                        render()
                    }
                }
                tile.addView(icon(R.drawable.ic_center_monitor, tile = true))
                val name = column()
                name.addView(label(0, 15, bold = true).apply { text = device.name })
                name.addView(status(deviceStatus(device.online), device.online))
                tile.addView(name, LinearLayout.LayoutParams(0, -2, 1f))
                tile.addView(chevron())
                content.addView(tile, LinearLayout.LayoutParams(-1, -2))
                tile.minimumHeight = dp(68)
                gap(content, 9)
            }
        }
        gap(content, 14)
        content.addView(button(R.string.center_local_network, listener = localNetwork))
        val tabs = row().apply { setPadding(dp(8), 0, dp(8), 0) }
        tabs.addView(tab(R.string.center_nav_devices, R.drawable.ic_center_monitor, active = true, listener = refresh),
            LinearLayout.LayoutParams(0, -2, 1f))
        tabs.addView(tab(R.string.center_nav_center, R.drawable.ic_center_server, listener = centerSettings),
            LinearLayout.LayoutParams(0, -2, 1f))
        tabs.addView(tab(R.string.center_settings, R.drawable.ic_center_sliders, listener = streamSettings),
            LinearLayout.LayoutParams(0, -2, 1f))
        footer(tabs)
    }

    private fun showDetails(content: LinearLayout, device: CenterDirectory.Device) {
        content.addView(label(R.string.center_my_devices, 14, bold = true, color = accent).apply {
            gravity = Gravity.CENTER_VERTICAL
            minHeight = dp(48)
            setPadding(dp(4), 0, dp(10), 0)
            compoundDrawablePadding = dp(5)
            setCompoundDrawablesRelative(glyph(R.drawable.ic_arrow_back_24, 16, accent), null, null, null)
            action(Color.TRANSPARENT, stroke = null) { backToDevices() }
        }, LinearLayout.LayoutParams(-2, -2))
        gap(content, 8)
        val heading = row()
        heading.addView(label(0, 26, bold = true).apply { text = device.name }, LinearLayout.LayoutParams(0, -2, 1f))
        heading.addView(status(deviceStatus(device.online), device.online).apply {
            setPadding(dp(9), dp(4), dp(9), dp(4))
            background = shape(if (device.online) greenFill else sky, radius = 7)
        }, LinearLayout.LayoutParams(-2, -2).apply { marginStart = dp(10) })
        content.addView(heading)
        gap(content, 16)
        val desktop = column().apply {
            background = shape(surface, line, radius = 14)
            clipToOutline = true
        }
        desktop.addView(ImageView(context).apply {
            scaleType = ImageView.ScaleType.CENTER_CROP
            setImageResource(R.drawable.center_device_cover)
            importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
        }, LinearLayout.LayoutParams(-1, dp(171)))
        desktop.addView(row().apply {
            setPadding(dp(12), 0, dp(12), 0)
            addView(icon(R.drawable.ic_center_monitor))
            addView(label(R.string.center_remote_desktop, 15, bold = true))
        }, LinearLayout.LayoutParams(-1, dp(49)))
        content.addView(desktop)
        gap(content, 20)
        content.addView(label(R.string.center_common_actions, 13, color = muted))
        gap(content, 10)
        val ready = device.online && !loading
        addDetailAction(content, R.string.center_app_list, R.drawable.ic_center_apps, ready) { deviceAction(device, Action.APPS) }
        addDetailAction(content, R.string.center_secondary_screen, R.drawable.ic_screen_mode_secondary, ready) {
            deviceAction(device, Action.SECONDARY_SCREEN)
        }
        addDetailAction(content, R.string.center_stream_settings, R.drawable.ic_center_sliders, true, streamSettings)
        addDetailAction(content, R.string.center_device_options, R.drawable.ic_more_vert, ready) {
            deviceAction(device, Action.DEVICE_OPTIONS)
        }
        val bar = row().apply { setPadding(dp(20), dp(10), dp(20), dp(10)) }
        bar.addView(button(R.string.center_connect_desktop, primary = true, enabled = ready) {
            deviceAction(device, Action.DESKTOP)
        }, LinearLayout.LayoutParams(-1, -2))
        footer(bar)
    }

    private fun addDetailAction(content: LinearLayout, name: Int, icon: Int, enabled: Boolean, listener: () -> Unit) {
        content.addView(button(name, enabled = enabled, listener = listener).apply {
            gravity = Gravity.CENTER_VERTICAL or Gravity.START
            setPadding(dp(12), dp(7), dp(12), dp(7))
            compoundDrawablePadding = dp(10)
            setCompoundDrawablesRelative(glyph(icon, 20, if (enabled) accent else muted), null,
                glyph(R.drawable.ic_arrow_right, 16, muted), null)
        }, LinearLayout.LayoutParams(-1, -2))
        gap(content, 8)
    }

    private fun footer(content: View) {
        root.addView(column().apply {
            setBackgroundColor(surface)
            addView(View(context).apply { setBackgroundColor(line) }, LinearLayout.LayoutParams(-1, dp(1)))
            addView(content, LinearLayout.LayoutParams(-1, -2))
        })
    }

    private fun deviceStatus(online: Boolean) = if (online) R.string.center_online else R.string.center_offline_short

    private fun status(textId: Int, online: Boolean) = label(textId, 13, color = if (online) green else muted).apply {
        val dot = shape(if (online) green else muted).apply { setBounds(0, 0, dp(6), dp(6)) }
        compoundDrawablePadding = dp(6)
        setCompoundDrawablesRelative(dot, null, null, null)
    }

    private fun column() = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
    private fun row() = LinearLayout(context).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
    }

    private fun label(textId: Int, size: Int, bold: Boolean = false, color: Int = ink) = TextView(context).apply {
        if (textId != 0) setText(textId)
        textSize = size.toFloat()
        setTextColor(color)
        if (bold) typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
    }

    private fun button(textId: Int, primary: Boolean = false, enabled: Boolean = true, listener: () -> Unit) =
        label(textId, 14, bold = true, color = if (primary && enabled) onAccent else if (enabled) ink else muted)
            .apply {
                gravity = Gravity.CENTER
                minHeight = dp(48)
                setPadding(dp(10), 0, dp(10), 0)
                if (enabled) action(if (primary) accent else surface, stroke = if (primary) null else line, listener = listener)
                else background = shape(sky, line)
                isEnabled = enabled
            }

    private fun tab(textId: Int, drawable: Int, active: Boolean = false, listener: () -> Unit) =
        label(textId, 12, bold = active, color = if (active) accent else muted).apply {
            gravity = Gravity.CENTER
            minHeight = dp(54)
            setPadding(dp(6), dp(6), dp(6), dp(6))
            compoundDrawablePadding = dp(4)
            setCompoundDrawablesRelative(null, glyph(drawable, 22, if (active) accent else muted), null, null)
            isSelected = active
            action(Color.TRANSPARENT, stroke = null, listener = listener)
        }

    private fun icon(drawable: Int, tile: Boolean = false) = ImageView(context).apply {
        val size = if (tile) 40 else 22
        layoutParams = LinearLayout.LayoutParams(dp(size), dp(size)).apply { marginEnd = dp(10) }
        if (tile) {
            setPadding(dp(9), dp(9), dp(9), dp(9))
            background = shape(sky)
        }
        setImageResource(drawable)
        imageTintList = ColorStateList.valueOf(accent)
        importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
    }

    private fun chevron() = ImageView(context).apply {
        layoutParams = LinearLayout.LayoutParams(dp(16), dp(16)).apply { marginStart = dp(8) }
        setImageDrawable(glyph(R.drawable.ic_arrow_right, 16, muted))
        importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
    }

    private fun glyph(drawable: Int, size: Int, tint: Int): Drawable? =
        ContextCompat.getDrawable(context, drawable)?.mutate()?.apply {
            setBounds(0, 0, dp(size), dp(size))
            setTint(tint)
        }

    private fun View.action(fill: Int, stroke: Int? = line, listener: () -> Unit) {
        background = RippleDrawable(rippleColor, shape(fill, stroke), shape(Color.WHITE))
        isFocusable = true
        stateListAnimator = null
        setOnClickListener { listener() }
        accessibilityDelegate = object : View.AccessibilityDelegate() {
            override fun onInitializeAccessibilityNodeInfo(host: View, info: AccessibilityNodeInfo) {
                super.onInitializeAccessibilityNodeInfo(host, info)
                info.className = Button::class.java.name
            }
        }
    }

    private fun shape(fill: Int, stroke: Int? = null, radius: Int = 10) = GradientDrawable().apply {
        setColor(fill)
        cornerRadius = dp(radius).toFloat()
        if (stroke != null) setStroke(dp(1), stroke)
    }

    private fun gap(parent: LinearLayout, size: Int) {
        parent.addView(View(context), LinearLayout.LayoutParams(1, dp(size)))
    }

    private fun dp(value: Int) = (value * resources.displayMetrics.density).roundToInt()

    private fun color(resource: Int) = ContextCompat.getColor(palette, resource)
}
