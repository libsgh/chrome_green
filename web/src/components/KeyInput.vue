<template>
  <div class="key-input-wrap">
    <button
      type="button"
      :class="['key-input', { recording: isRecording }]"
      @click="startRecording"
      @blur="stopRecording">
      <span v-if="isRecording" class="key-input-rec">{{ t("key_recording") }}</span>
      <span v-else-if="modelValue" class="key-input-val">{{ modelValue }}</span>
      <span v-else class="key-input-ph">{{ placeholder }}</span>
    </button>
    <button
      v-if="modelValue && !isRecording"
      type="button"
      class="key-input-clear"
      :title="t('keymapping_remove')"
      @click="clearValue">
      <Icon name="close" :size="14" />
    </button>
  </div>
</template>

<script setup>
import { ref, onBeforeUnmount } from "vue";
import Icon from "./Icon.vue";
import { t } from "../i18n.js";
import { useStore } from "../store.js";
import { api } from "../api.js";

const { setKeyCaptureCallback } = useStore();

const props = defineProps({
  modelValue: { type: String, default: "" },
  placeholder: { type: String, default: "" },
});
const emit = defineEmits(["update:modelValue", "change"]);

const isRecording = ref(false);
let stopTimer = null;
let fallbackCleanup = null;

function startRecording() {
  if (isRecording.value) return;
  isRecording.value = true;

  // Register the SSE callback that receives the captured combo from the
  // backend (which swallows Chrome's own shortcuts during capture).
  setKeyCaptureCallback(onCaptured);

  // Safety: auto-abort if no key arrives within 15s.
  stopTimer = setTimeout(stopRecording, 15000);

  api
    .startCapture()
    .catch(() => {
      // Backend capture unavailable, so fall back to a plain window listener;
      // the input still works for non-conflicting keys.
      startFallbackCapture();
    });
}

function onCaptured(data) {
  stopRecording();
  if (!data || data.cancel) return;
  if (data.value) {
    emit("update:modelValue", data.value);
    emit("change", data.value);
  }
}

// Fallback: capture keys on the window (no Chrome-shortcut suppression).
function startFallbackCapture() {
  const onKeydown = (e) => {
    e.preventDefault();
    e.stopPropagation();
    if (e.key === "Escape") {
      stopRecording();
      return;
    }
    const mods = [];
    if (e.ctrlKey) mods.push("Ctrl");
    if (e.altKey) mods.push("Alt");
    if (e.shiftKey) mods.push("Shift");
    if (e.metaKey) mods.push("Win");
    mods.push(e.key.length === 1 ? e.key.toUpperCase() : e.key);
    const combo = mods.join("+");
    emit("update:modelValue", combo);
    emit("change", combo);
    stopRecording();
  };
  window.addEventListener("keydown", onKeydown, true);
  fallbackCleanup = () => window.removeEventListener("keydown", onKeydown, true);
}

function stopRecording() {
  if (!isRecording.value) return;
  isRecording.value = false;
  if (stopTimer) {
    clearTimeout(stopTimer);
    stopTimer = null;
  }
  if (fallbackCleanup) {
    fallbackCleanup();
    fallbackCleanup = null;
  }
  setKeyCaptureCallback(null);
  api.stopCapture().catch(() => {});
}

function clearValue() {
  emit("update:modelValue", "");
  emit("change", "");
}

onBeforeUnmount(stopRecording);
</script>

<style scoped>
.key-input-wrap {
  display: flex;
  align-items: center;
  gap: 4px;
}
.key-input {
  flex: 1;
  min-width: 0;
  text-align: left;
  font-family: inherit;
  font-size: 13px;
  padding: 7px 10px;
  border-radius: 8px;
  border: 1px solid hsl(var(--border));
  background: hsl(var(--background));
  color: hsl(var(--foreground));
  cursor: pointer;
  transition: border-color 0.15s, box-shadow 0.15s;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  outline: none;
}
.key-input:focus-visible {
  outline: 2px solid hsl(var(--ring));
  outline-offset: 2px;
}
.key-input:hover {
  border-color: hsl(var(--primary));
}
.key-input.recording {
  border-color: hsl(var(--primary));
  box-shadow: 0 0 0 3px hsl(var(--primary) / 0.25);
}
.key-input-rec {
  color: hsl(var(--primary));
  font-style: italic;
}
.key-input-val {
  color: hsl(var(--foreground));
  font-weight: 600;
}
.key-input-ph {
  color: hsl(var(--muted-foreground));
}
.key-input-clear {
  flex: none;
  width: 28px;
  height: 28px;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  border-radius: 6px;
  border: 1px solid hsl(var(--border));
  background: hsl(var(--background));
  color: hsl(var(--muted-foreground));
  cursor: pointer;
}
.key-input-clear:hover {
  color: hsl(var(--foreground));
  border-color: hsl(var(--primary));
}
</style>
