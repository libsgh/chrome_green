<template>
  <div class="card">
    <div class="card-header flex flex-row items-center justify-between">
      <div>
        <h2 class="card-title">{{ t("debug_log") }}</h2>
        <p class="card-description">{{ t("debug_log_desc") }}</p>
      </div>
      <div class="flex gap-2">
        <button class="btn btn-outline text-xs px-2 py-1" @click="copyLogs">
          {{ logCopied ? t("log_copied") : t("log_copy") }}
        </button>
        <button
          class="btn btn-outline text-xs px-2 py-1"
          @click="clearLogs">
          {{ t("log_clear") }}
        </button>
      </div>
    </div>
    <div class="card-content">
      <div
        v-if="logs.length === 0"
        class="text-xs text-[hsl(var(--muted-foreground))] py-4 text-center">
        {{ t("log_empty") }}
      </div>
      <div
        v-else
        class="log-container rounded-md bg-[hsl(var(--secondary))] border border-[hsl(var(--border))] p-3 max-h-80 overflow-y-auto">
        <pre
          class="text-xs font-mono leading-relaxed whitespace-pre-wrap text-[hsl(var(--foreground))] m-0"><code>{{ logs.join('\n') }}</code></pre>
      </div>
    </div>
  </div>
</template>

<script setup>
import { useStore } from "../../store.js";
import { t } from "../../i18n.js";

const { logs, logCopied, loadLogs, copyLogs, clearLogs } = useStore();
</script>
