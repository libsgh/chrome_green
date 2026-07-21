<template>
  <div class="card">
    <div class="card-header">
      <h2 class="card-title">{{ t("proxy_settings") }}</h2>
      <p class="card-description">{{ t("proxy_settings_desc") }}</p>
    </div>
    <div class="card-content space-y-0">
      <div class="setting-row">
        <div class="setting-label-group">
          <label class="setting-label">{{ t("proxy_type") }}</label>
          <p class="setting-desc">{{ t("proxy_type_desc") }}</p>
        </div>
        <div class="w-32">
          <CustomSelect
            v-model="settings.proxy_type"
            :options="proxyTypeOptions"
            @change="() => saveSettings()" />
        </div>
      </div>
      <div class="setting-row">
        <div class="setting-label-group">
          <label class="setting-label">{{ t("proxy_address") }}</label>
          <p class="setting-desc">{{ t("proxy_address_desc") }}</p>
        </div>
        <div class="w-64">
          <input
            v-model="settings.proxy"
            @blur="saveSettings"
            class="input"
            :placeholder="t('proxy_placeholder')" />
        </div>
      </div>
      <div class="setting-row">
        <div class="setting-label-group">
          <label class="setting-label">{{ t("proxy_chrome_download") }}</label>
          <p class="setting-desc">{{ t("proxy_chrome_download_desc") }}</p>
        </div>
        <div
          :class="[
            'switch-track',
            settings.proxy_chrome_download ? 'on' : 'off',
          ]"
          :style="
            ghProxy
              ? { opacity: 0.45, pointerEvents: 'none', cursor: 'not-allowed' }
              : {}
          "
          :aria-disabled="ghProxy"
          @click="ghProxy ? null : toggle('proxy_chrome_download')"
          role="switch"
          :aria-checked="settings.proxy_chrome_download">
          <span class="switch-thumb"></span>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { computed } from "vue";
import { t } from "../../i18n.js";
import { useStore } from "../../store.js";
import CustomSelect from "../CustomSelect.vue";

const { settings, proxyTypeOptions, saveSettings, toggle } = useStore();
const ghProxy = computed(() => settings.proxy_type === "GH_PROXY");
</script>
