package com.fingerprintlock.app.ui

import android.content.Context
import android.view.LayoutInflater
import android.view.View
import androidx.annotation.DrawableRes
import com.fingerprintlock.app.R
import com.fingerprintlock.app.databinding.DialogAppBinding
import com.google.android.material.dialog.MaterialAlertDialogBuilder

object AppDialogs {
    data class Result(val confirmed: Boolean, val input: String)

    fun confirm(
        context: Context,
        title: String,
        message: String? = null,
        confirmText: String = "确定",
        cancelText: String = "取消",
        @DrawableRes iconRes: Int = R.drawable.ic_nav_home_on,
        danger: Boolean = false,
        onConfirm: () -> Unit
    ) {
        val b = DialogAppBinding.inflate(LayoutInflater.from(context))
        b.tvDialogTitle.text = title
        if (message.isNullOrBlank()) {
            b.tvDialogMessage.visibility = View.GONE
        } else {
            b.tvDialogMessage.visibility = View.VISIBLE
            b.tvDialogMessage.text = message
        }
        b.tilDialogInput.visibility = View.GONE
        b.ivDialogIcon.setImageResource(iconRes)
        b.btnDialogConfirm.text = confirmText
        b.btnDialogCancel.text = cancelText
        if (danger) {
            b.btnDialogConfirm.setBackgroundColor(context.getColor(R.color.danger))
        }
        val dialog = MaterialAlertDialogBuilder(context)
            .setView(b.root)
            .create()
        dialog.window?.setBackgroundDrawableResource(R.drawable.bg_dialog_surface)
        b.btnDialogCancel.setOnClickListener { dialog.dismiss() }
        b.btnDialogConfirm.setOnClickListener {
            dialog.dismiss()
            onConfirm()
        }
        dialog.show()
    }

    fun input(
        context: Context,
        title: String,
        message: String? = null,
        hint: String = "备注",
        initial: String = "",
        confirmText: String = "确定",
        @DrawableRes iconRes: Int = R.drawable.ic_nav_fp_on,
        onConfirm: (String) -> Unit
    ) {
        val b = DialogAppBinding.inflate(LayoutInflater.from(context))
        b.tvDialogTitle.text = title
        if (message.isNullOrBlank()) {
            b.tvDialogMessage.visibility = View.GONE
        } else {
            b.tvDialogMessage.visibility = View.VISIBLE
            b.tvDialogMessage.text = message
        }
        b.tilDialogInput.visibility = View.VISIBLE
        b.tilDialogInput.hint = hint
        b.etDialogInput.setText(initial)
        b.etDialogInput.setSelection(b.etDialogInput.text?.length ?: 0)
        b.ivDialogIcon.setImageResource(iconRes)
        b.btnDialogConfirm.text = confirmText
        val dialog = MaterialAlertDialogBuilder(context)
            .setView(b.root)
            .create()
        dialog.window?.setBackgroundDrawableResource(R.drawable.bg_dialog_surface)
        b.btnDialogCancel.setOnClickListener { dialog.dismiss() }
        b.btnDialogConfirm.setOnClickListener {
            val text = b.etDialogInput.text?.toString()?.trim().orEmpty()
            dialog.dismiss()
            onConfirm(text)
        }
        dialog.show()
    }
}
