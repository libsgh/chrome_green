<template>
  <div class="card">
    <div class="card-header">
      <h2 class="card-title">{{ t("update_settings") }}</h2>
      <p class="card-description">{{ t("update_settings_desc") }}</p>
    </div>
    <div class="card-content space-y-0">
      <div class="setting-row">
        <div class="setting-label-group">
          <label class="setting-label">{{ t("release_channel") }}</label>
          <p class="setting-desc">{{ t("release_channel_desc") }}</p>
        </div>
        <div class="w-36">
          <CustomSelect
            v-model="settings.channel"
            :options="channelOptions"
            @change="() => saveSettings(true)" />
        </div>
      </div>

      <div class="setting-row">
        <div class="setting-label-group">
          <label class="setting-label">{{ t("download_source") }}</label>
          <p class="setting-desc">{{ t("download_source_desc") }}</p>
        </div>
        <div class="flex flex-col gap-1.5">
          <label class="flex items-center gap-2 cursor-pointer text-sm">
            <input
              type="radio"
              v-model="settings.download_source"
              :value="0"
              @change="saveSettings(true)"
              class="radio" />
            <span>edgedl.me.gvt1.com</span>
          </label>
          <label class="flex items-center gap-2 cursor-pointer text-sm">
            <input
              type="radio"
              v-model="settings.download_source"
              :value="1"
              @change="saveSettings(true)"
              class="radio" />
            <span>dl.google.com</span>
          </label>
          <label class="flex items-center gap-2 cursor-pointer text-sm">
            <input
              type="radio"
              v-model="settings.download_source"
              :value="2"
              @change="saveSettings(true)"
              class="radio" />
            <span>www.google.com</span>
          </label>
          <label class="flex items-center gap-2 cursor-pointer text-sm">
            <input
              type="radio"
              v-model="settings.download_source"
              :value="3"
              @change="saveSettings(true)"
              class="radio" />
            <span>redirector.gvt1.com</span>
          </label>
        </div>
      </div>

      <div class="setting-row">
        <div class="setting-label-group">
          <label class="setting-label">{{ t("auto_check") }}</label>
          <p class="setting-desc">{{ t("auto_check_desc") }}</p>
        </div>
        <div
          :class="['switch-track', settings.auto_check ? 'on' : 'off']"
          @click="toggle('auto_check')"
          role="switch"
          :aria-checked="settings.auto_check">
          <span class="switch-thumb"></span>
        </div>
      </div>

      <div v-if="settings.auto_check" class="setting-row">
        <div class="setting-label-group">
          <label class="setting-label">{{ t("check_interval") }}</label>
          <p class="setting-desc">{{ t("check_interval_desc") }}</p>
        </div>
        <div class="w-28">
          <CustomSelect
            v-model="settings.check_interval"
            :options="checkIntervalOptions"
            @change="() => saveSettings()" />
        </div>
      </div>

      <div class="setting-row">
        <div class="setting-label-group">
          <label class="setting-label">{{ t("auto_download") }}</label>
          <p class="setting-desc">{{ t("auto_download_desc") }}</p>
        </div>
        <div
          :class="['switch-track', settings.auto_download ? 'on' : 'off']"
          @click="toggle('auto_download')"
          role="switch"
          :aria-checked="settings.auto_download">
          <span class="switch-thumb"></span>
        </div>
      </div>

      <div class="setting-row">
        <div class="setting-label-group">
          <label class="setting-label">{{ t("keep_installer") }}</label>
          <p class="setting-desc">{{ t("keep_installer_desc") }}</p>
        </div>
        <div
          :class="['switch-track', settings.keep_installer ? 'on' : 'off']"
          @click="toggle('keep_installer')"
          role="switch"
          :aria-checked="settings.keep_installer">
          <span class="switch-thumb"></span>
        </div>
      </div>

      <div class="setting-row">
        <div class="setting-label-group">
          <label class="setting-label">{{ t("keep_old_versions") }}</label>
          <p class="setting-desc">{{ t("keep_old_versions_desc") }}</p>
        </div>
        <div
          :class="[
            'switch-track',
            settings.keep_old_versions ? 'on' : 'off',
          ]"
          @click="toggle('keep_old_versions')"
          role="switch"
          :aria-checked="settings.keep_old_versions">
          <span class="switch-thumb"></span>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import CustomSelect from "../CustomSelect.vue";
import { useStore } from "../../store.js";
import { t } from "../../i18n.js";

const { settings, channelOptions, checkIntervalOptions, saveSettings, toggle } =
  useStore();
</script>
