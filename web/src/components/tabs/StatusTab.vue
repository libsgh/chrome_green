<template>
  <div class="card">
    <div class="card-header">
      <h2 class="card-title">{{ t("update_status") }}</h2>
      <p class="card-description">{{ t("update_status_desc") }}</p>
    </div>
    <div class="card-content space-y-4">
      <!-- Version / channel summary: 3 columns in one row -->
      <div class="grid grid-cols-3 gap-4">
        <!-- 1. Installed version (with architecture) -->
        <div class="space-y-1 min-w-0">
          <p class="text-xs text-[hsl(var(--muted-foreground))]">
            {{ t("installed_version") }}
          </p>
          <p class="text-sm font-mono font-medium truncate">
            {{ status.current_version || "Unknown" }} ({{ status.arch }})
          </p>
        </div>

        <!-- 2. Update channel -->
        <div class="space-y-1 min-w-0">
          <p class="text-xs text-[hsl(var(--muted-foreground))]">
            {{ t("update_channel") }}
          </p>
          <p class="text-sm font-medium capitalize truncate">
            {{ status.channel }}
          </p>
        </div>

        <!-- 3. ChromeGreen self-update (last column) -->
        <div class="space-y-1 min-w-0">
          <p class="text-xs text-[hsl(var(--muted-foreground))]">
            {{ t("self_current") }}
          </p>
          <div class="flex items-center gap-2 min-w-0">
            <p class="text-sm font-mono font-medium truncate">{{ version }}</p>
            <button
              v-if="
                (hasChecked && selfHasUpdate) ||
                status.self_update_ready ||
                selfDownloading
              "
              class="btn btn-tiny inline-flex items-center gap-1 bg-primary text-primary-foreground hover:opacity-90 shadow-md shadow-black/20 font-semibold shrink-0"
              :disabled="selfUpdateDisabled"
              @click="onSelfUpdateClick">
              <span
                v-if="selfDownloading && !status.self_update_ready"
                class="inline-block w-3 h-3 border-2 border-primary-foreground/40 border-t-primary-foreground rounded-full animate-spin align-middle -mt-px"></span>
              <svg
                v-else
                class="w-3 h-3"
                viewBox="0 0 24 24"
                fill="none"
                stroke="currentColor"
                stroke-width="2.25"
                stroke-linecap="round"
                stroke-linejoin="round">
                <circle cx="12" cy="12" r="10" />
                <path d="m16 12-4-4-4 4" />
                <path d="M12 16V8" />
              </svg>
              {{ selfUpdateBtnText }}
            </button>
          </div>
        </div>
      </div>

      <!-- 5. Last check + status/actions -->
      <div class="flex items-center justify-between gap-3 flex-wrap">
        <div class="space-y-1">
          <p class="text-xs text-[hsl(var(--muted-foreground))]">
            {{ t("last_check") }}
          </p>
          <p class="text-sm font-medium">
            {{ formatTime(status.last_check_time) }}
          </p>
        </div>
        <div class="flex items-center gap-3 flex-wrap">
          <span :class="statusBadgeClass">{{ statusLabel }}</span>
          <button
            class="btn inline-flex items-center gap-1.5 border border-border text-foreground hover:bg-accent"
            @click="checkAllUpdates"
            :disabled="isAutoChecking || selfChecking || updateInProgress"
            :title="
              updateInProgress ? t('check_disabled_downloading') : undefined
            ">
            <span
              v-if="isAutoChecking || selfChecking"
              class="inline-block w-3.5 h-3.5 border-2 border-foreground/30 border-t-foreground rounded-full animate-spin align-middle -mt-px"></span>
            <svg
              v-else
              class="w-4 h-4"
              viewBox="0 0 24 24"
              fill="none"
              stroke="currentColor"
              stroke-width="2"
              stroke-linecap="round"
              stroke-linejoin="round">
              <path d="M21 12a9 9 0 0 0-9-9 9.75 9.75 0 0 0-6.74 2.74L3 8" />
              <path d="M3 3v5h5" />
              <path d="M3 12a9 9 0 0 0 9 9 9.75 9.75 0 0 0 6.74-2.74L21 16" />
              <path d="M21 21v-5h-5" />
            </svg>
            {{
              isAutoChecking || selfChecking
                ? t("checking_btn")
                : t("check_update")
            }}
          </button>
        </div>
      </div>

      <!-- Other action buttons -->
      <div class="flex items-center gap-3 pt-2 flex-wrap">
        <button
          v-if="status.state === 'idle' && status.has_local_package"
          class="btn btn-outline"
          @click="offlineInstall"
          :disabled="downloading">
          {{ downloading ? t("starting") : t("offline_install") }}
        </button>
        <button
          v-if="status.state === 'idle' && status.has_pending_swap"
          class="btn btn-primary"
          @click="applyAndRestart">
          {{ t("complete_swap") }}
        </button>
        <button
          v-if="status.state === 'available' && !isAutoChecking && hasChecked"
          class="btn btn-primary inline-flex items-center gap-1.5"
          @click="startDownload"
          :disabled="downloading">
          <svg
            class="w-4 h-4"
            viewBox="0 0 24 24"
            fill="none"
            stroke="currentColor"
            stroke-width="2"
            stroke-linecap="round"
            stroke-linejoin="round">
            <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
            <polyline points="7 10 12 15 17 10" />
            <line x1="12" y1="15" x2="12" y2="3" />
          </svg>
          {{ downloading ? t("starting") : t("download_update") }}
        </button>
        <button
          v-if="status.state === 'downloading'"
          class="btn btn-destructive"
          @click="cancelDownload">
          {{ t("cancel") }}
        </button>
        <button
          v-if="status.state === 'ready' || status.state === 'pending_apply'"
          class="btn btn-primary inline-flex items-center gap-1.5"
          @click="applyAndRestart">
          <svg
            class="w-3 h-3"
            viewBox="0 0 24 24"
            fill="none"
            stroke="currentColor"
            stroke-width="2.25"
            stroke-linecap="round"
            stroke-linejoin="round">
            <circle cx="12" cy="12" r="10" />
            <path d="m16 12-4-4-4 4" />
            <path d="M12 16V8" />
          </svg>
          {{ t("restart_apply") }}
        </button>
        <button
          v-if="status.state === 'ready' || status.state === 'error'"
          class="btn btn-outline text-xs"
          @click="resetUpdate">
          {{ t("reset_state") }}
        </button>
      </div>

      <!-- Update info banner -->
      <div
        v-if="
          (hasChecked &&
            (status.state === 'available' || status.state === 'downloading')) ||
          status.state === 'ready' ||
          status.state === 'pending_apply'
        "
        class="update-banner rounded-md border p-4 space-y-1">
        <p class="text-sm font-semibold">{{ t("update_available") }}</p>
        <p class="text-sm">
          {{ t("new_version") }}
          <span class="font-mono font-medium">{{ status.latest_version }}</span>
        </p>
        <p v-if="status.download_size > 0" class="text-sm">
          {{ t("size") }} {{ formatBytes(status.download_size) }}
        </p>
        <p v-if="status.sha256" class="text-sm">SHA-256: {{ status.sha256 }}</p>
      </div>

      <!-- Error -->
      <div
        v-if="status.state === 'error' && status.error_message"
        class="error-banner rounded-md border p-4">
        <p class="text-sm font-medium">Error</p>
        <p class="text-sm font-mono mt-1">{{ status.error_message }}</p>
      </div>

      <!-- Local package available banner -->
      <div
        v-if="status.has_local_package && status.state === 'idle'"
        class="rounded-md border border-[hsl(var(--border))] bg-[hsl(var(--secondary))] p-4 space-y-1">
        <p class="text-sm font-medium">{{ t("local_package_found") }}</p>
        <p class="text-sm text-[hsl(var(--muted-foreground))]">
          {{ t("local_package_desc") }}
        </p>
      </div>

      <!-- Pending swap banner (chrome.exe.new exists from failed update) -->
      <div
        v-if="status.has_pending_swap && status.state === 'idle'"
        class="update-banner rounded-md border p-4 space-y-1">
        <p class="text-sm font-medium">{{ t("pending_swap_title") }}</p>
        <p class="text-sm text-[hsl(var(--muted-foreground))]">
          {{ t("pending_swap_desc") }}
        </p>
      </div>

      <!-- Progress (live via SSE) -->
      <div v-if="status.state === 'downloading'" class="space-y-2">
        <div class="progress-track">
          <div
            class="progress-fill"
            :class="status.download_progress >= 100 ? 'success' : 'info'"
            :style="{ width: status.download_progress + '%' }"></div>
        </div>
        <div
          class="flex items-center justify-between gap-3 text-xs text-[hsl(var(--muted-foreground))]">
          <span class="font-semibold text-[hsl(var(--foreground))] tabular-nums"
            >{{ status.download_progress }}%</span
          >
          <span class="tabular-nums"
            >{{ formatBytes(status.downloaded_bytes) }} /
            {{ formatBytes(status.download_size) }}</span
          >
          <span
            class="tabular-nums font-medium text-[hsl(var(--foreground))]"
            >{{ formatSpeed(status.download_speed) }}</span
          >
          <span class="tabular-nums">{{ t("remaining") }} {{ etaText }}</span>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { computed } from "vue";
import { t } from "../../i18n.js";
import { useStore } from "../../store.js";

const {
  status,
  downloading,
  isAutoChecking,
  hasChecked,
  etaText,
  statusLabel,
  statusBadgeClass,
  version,
  formatTime,
  formatBytes,
  formatSpeed,
  checkAllUpdates,
  startDownload,
  offlineInstall,
  cancelDownload,
  resetUpdate,
  applyAndRestart,
  selfHasUpdate,
  selfDownloading,
  selfChecking,
  downloadSelfUpdate,
  applySelfUpdate,
} = useStore();

const selfUpdateBtnText = computed(() => {
  if (status.self_update_ready) return t("self_restart");
  if (selfDownloading.value)
    return status.self_download_progress > 0
      ? status.self_download_progress + "%"
      : t("self_downloading");
  if (selfHasUpdate.value) return t("self_update_available");
  return "";
});

const selfUpdateDisabled = computed(() => {
  return selfDownloading.value && !status.self_update_ready;
});

// While a download (or the post-download extraction) is running, a new check
// would clobber the live state — the check button stays disabled until the
// user cancels or the download finishes.
const updateInProgress = computed(
  () => status.state === "downloading" || status.state === "applying",
);

async function onSelfUpdateClick() {
  if (status.self_update_ready) {
    await applySelfUpdate();
  } else if (selfHasUpdate.value && !selfDownloading.value) {
    await downloadSelfUpdate();
  }
}
</script>

<style scoped>
/* 状态页按钮：更紧凑精致（仅本组件生效，不影响其他页面） */
.btn {
  height: 2rem; /* h-8: 32px */
  padding: 0.375rem 0.75rem; /* px-3 py-1.5 */
  font-size: 0.8125rem; /* 13px */
  border-radius: 0.5rem; /* rounded-md */
}
.btn svg {
  width: 0.875rem; /* w-3.5 */
  height: 0.875rem;
}
/* 复位等辅助按钮保留更小的字号层级 */
.btn.text-xs {
  font-size: 0.75rem;
}
/* 自更新按钮 tiny 变体：比默认 .btn 更小 */
.btn-tiny {
  height: 1.25rem; /* h-5: 20px */
  padding: 0.125rem 0.5rem; /* px-2 py-0.5 */
  font-size: 0.75rem; /* 12px */
  border-radius: 0.375rem; /* rounded-md */
}
.btn-tiny svg {
  width: 0.75rem; /* w-3 */
  height: 0.75rem;
}
</style>
