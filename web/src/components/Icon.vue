<template>
  <span
    v-if="iconData.svg"
    :class="['inline-flex items-center justify-center leading-none', className]"
    :style="{ fontSize: typeof size === 'number' ? `${size}px` : size }"
    :aria-hidden="ariaHidden"
    role="img"
    v-bind="$attrs">
    <span class="block leading-none" v-html="iconData.svg" />
  </span>
  <svg
    v-else
    :class="className"
    :width="size"
    :height="size"
    :viewBox="iconData.viewBox"
    :fill="fill"
    :stroke="stroke"
    :stroke-width="strokeWidth"
    :stroke-linecap="strokeLinecap"
    :stroke-linejoin="strokeLinejoin"
    :aria-hidden="ariaHidden"
    role="img"
    v-bind="$attrs">
    <component
      v-for="(node, index) in iconData.nodes"
      :key="`${name}-${index}`"
      :is="node.tag"
      v-bind="node.attrs" />
  </svg>
</template>

<script setup>
import { computed } from "vue";
import { iconRegistry } from "../icons.js";

const props = defineProps({
  name: {
    type: String,
    required: true,
  },
  size: {
    type: [String, Number],
    default: 16,
  },
  className: {
    type: [String, Array, Object],
    default: "",
  },
  fill: {
    type: String,
    default: "none",
  },
  stroke: {
    type: String,
    default: "currentColor",
  },
  strokeWidth: {
    type: [String, Number],
    default: 1.8,
  },
  strokeLinecap: {
    type: String,
    default: "round",
  },
  strokeLinejoin: {
    type: String,
    default: "round",
  },
  ariaHidden: {
    type: Boolean,
    default: true,
  },
});

const iconData = computed(() => iconRegistry[props.name] || iconRegistry.globe);
</script>
