<template>
  <div class="card">
    <div class="card-header">
      <h2 class="card-title">{{ t("tabs_title") }}</h2>
      <p class="card-description">{{ t("tabs_desc") }}</p>
    </div>
    <div class="card-content space-y-0">
      <!-- Keep last tab -->
      <div class="setting-row">
        <div class="setting-label-group">
          <label class="setting-label">{{ t("keep_last_tab") }}</label>
          <p class="setting-desc">{{ t("keep_last_tab_desc") }}</p>
        </div>
        <div
          :class="['switch-track', settings.keep_last_tab ? 'on' : 'off']"
          @click="toggle('keep_last_tab')"
          role="switch"
          :aria-checked="settings.keep_last_tab">
          <span class="switch-thumb"></span>
        </div>
      </div>

      <!-- Double-click close -->
      <div class="setting-row">
        <div class="setting-label-group">
          <label class="setting-label">{{ t("double_click_close") }}</label>
          <p class="setting-desc">{{ t("double_click_close_desc") }}</p>
        </div>
        <div
          :class="['switch-track', settings.double_click_close ? 'on' : 'off']"
          @click="toggle('double_click_close')"
          role="switch"
          :aria-checked="settings.double_click_close">
          <span class="switch-thumb"></span>
        </div>
      </div>

      <!-- Right-click close -->
      <div class="setting-row">
        <div class="setting-label-group">
          <label class="setting-label">{{ t("right_click_close") }}</label>
          <p class="setting-desc">{{ t("right_click_close_desc") }}</p>
        </div>
        <div
          :class="['switch-track', settings.right_click_close ? 'on' : 'off']"
          @click="toggle('right_click_close')"
          role="switch"
          :aria-checked="settings.right_click_close">
          <span class="switch-thumb"></span>
        </div>
      </div>

      <!-- Wheel switches tabs -->
      <div class="setting-row">
        <div class="setting-label-group">
          <label class="setting-label">{{ t("wheel_tab") }}</label>
          <p class="setting-desc">{{ t("wheel_tab_desc") }}</p>
        </div>
        <div
          :class="['switch-track', settings.wheel_tab ? 'on' : 'off']"
          @click="toggle('wheel_tab')"
          role="switch"
          :aria-checked="settings.wheel_tab">
          <span class="switch-thumb"></span>
        </div>
      </div>

      <!-- Wheel + right button switches tabs -->
      <div class="setting-row">
        <div class="setting-label-group">
          <label class="setting-label">{{ t("wheel_tab_when_press_rbutton") }}</label>
          <p class="setting-desc">{{ t("wheel_tab_when_press_rbutton_desc") }}</p>
        </div>
        <div
          :class="[
            'switch-track',
            settings.wheel_tab_when_press_rbutton ? 'on' : 'off',
          ]"
          @click="toggle('wheel_tab_when_press_rbutton')"
          role="switch"
          :aria-checked="settings.wheel_tab_when_press_rbutton">
          <span class="switch-thumb"></span>
        </div>
      </div>

      <!-- Hover to activate -->
      <div class="setting-row">
        <div class="setting-label-group">
          <label class="setting-label">{{ t("hover_tab") }}</label>
          <p class="setting-desc">{{ t("hover_tab_desc") }}</p>
        </div>
        <div
          :class="['switch-track', settings.hover_tab ? 'on' : 'off']"
          @click="toggle('hover_tab')"
          role="switch"
          :aria-checked="settings.hover_tab">
          <span class="switch-thumb"></span>
        </div>
      </div>

      <!-- Hover delay -->
      <div class="setting-row">
        <div class="setting-label-group">
          <label class="setting-label">{{ t("hover_tab_delay") }}</label>
          <p class="setting-desc">{{ t("hover_tab_delay_desc") }}</p>
        </div>
        <div class="w-32">
          <input
            type="number"
            min="0"
            max="5000"
            v-model.number="settings.hover_tab_delay"
            @blur="saveSettings"
            class="input" />
        </div>
      </div>

      <!-- Open URL in new tab -->
      <div class="setting-row">
        <div class="setting-label-group">
          <label class="setting-label">{{ t("open_url_new_tab") }}</label>
          <p class="setting-desc">{{ t("open_url_new_tab_desc") }}</p>
        </div>
        <div class="w-44">
          <CustomSelect
            v-model="settings.open_url_new_tab"
            :options="openUrlModeOptions"
            @change="() => saveSettings()" />
        </div>
      </div>

      <!-- Open bookmark in new tab -->
      <div class="setting-row">
        <div class="setting-label-group">
          <label class="setting-label">{{ t("open_bookmark_new_tab") }}</label>
          <p class="setting-desc">{{ t("open_bookmark_new_tab_desc") }}</p>
        </div>
        <div class="w-44">
          <CustomSelect
            v-model="settings.open_bookmark_new_tab"
            :options="bookmarkModeOptions"
            @change="() => saveSettings()" />
        </div>
      </div>

      <!-- Disable on named tabs -->
      <div class="setting-row">
        <div class="setting-label-group">
          <label class="setting-label">{{ t("new_tab_disable") }}</label>
          <p class="setting-desc">{{ t("new_tab_disable_desc") }}</p>
        </div>
        <div
          :class="['switch-track', settings.new_tab_disable ? 'on' : 'off']"
          @click="toggle('new_tab_disable')"
          role="switch"
          :aria-checked="settings.new_tab_disable">
          <span class="switch-thumb"></span>
        </div>
      </div>

      <!-- Excluded tab names -->
      <div class="setting-row">
        <div class="setting-label-group">
          <label class="setting-label">{{ t("new_tab_disable_name") }}</label>
          <p class="setting-desc">{{ t("new_tab_disable_name_desc") }}</p>
        </div>
        <div class="w-80 max-w-full">
          <input
            v-model="settings.new_tab_disable_name"
            @blur="saveSettings"
            class="input"
            :placeholder="t('new_tab_disable_name_desc')" />
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { computed } from "vue";
import CustomSelect from "../CustomSelect.vue";
import { useStore } from "../../store.js";
import { t } from "../../i18n.js";

const { settings, saveSettings, toggle } = useStore();

const openUrlModeOptions = computed(() => [
  { value: 0, label: t("mode_off") },
  { value: 1, label: t("mode_alt_enter") },
  { value: 2, label: t("mode_shift_alt_enter") },
]);

const bookmarkModeOptions = computed(() => [
  { value: 0, label: t("mode_off") },
  { value: 1, label: t("mode_middle_shift") },
  { value: 2, label: t("mode_middle") },
]);
</script>
