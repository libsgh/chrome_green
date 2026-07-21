<template>
  <div ref="root" class="relative w-full">
    <button
      ref="trigger"
      type="button"
      :disabled="disabled"
      :aria-expanded="isOpen"
      :aria-label="selectedLabel"
      class="flex h-9 w-full items-center justify-between rounded-lg border-2 border-[hsl(var(--border))] bg-[hsl(var(--card))] px-3 py-2 text-sm text-[hsl(var(--foreground))] shadow-[0_1px_2px_rgba(15,23,42,0.06)] transition-all duration-200 hover:border-[hsl(var(--primary))]/40 hover:shadow-[0_6px_16px_rgba(15,23,42,0.08)] focus-visible:outline-none focus-visible:border-[hsl(var(--primary))] focus-visible:shadow-[0_0_0_3px_hsl(var(--ring)/0.18)] disabled:cursor-not-allowed disabled:opacity-50"
      @click="toggleMenu"
      @keydown.esc.prevent="isOpen = false">
      <span class="truncate">{{ selectedLabel }}</span>
      <Icon
        name="chevronDown"
        :size="16"
        class-name="ml-2 h-4 w-4 shrink-0 text-[hsl(var(--muted-foreground))] transition-transform duration-150"
        :class="{ 'rotate-180': isOpen }" />
    </button>

    <div
      v-if="isOpen"
      class="absolute left-0 right-0 top-full z-20 mt-1 overflow-hidden rounded-xl border border-[hsl(var(--border))] bg-[hsl(var(--card))]/95 p-1 shadow-[0_12px_32px_rgba(15,23,42,0.16)] backdrop-blur-sm">
      <button
        v-for="option in options"
        :key="option.value"
        type="button"
        class="flex w-full items-center justify-between rounded-lg px-3 py-2 text-left text-sm transition-all duration-150"
        :class="
          selectedValue === option.value
            ? 'bg-[hsl(var(--primary))]/10 text-[hsl(var(--foreground))]'
            : 'text-[hsl(var(--muted-foreground))] hover:bg-[hsl(var(--accent))] hover:text-[hsl(var(--foreground))]'
        "
        @click="selectOption(option.value)">
        <span>{{ option.label }}</span>
        <Icon
          v-if="selectedValue === option.value"
          name="check"
          :size="16"
          class-name="h-4 w-4 text-[hsl(var(--primary))]"
          :stroke-width="2" />
      </button>
    </div>
  </div>
</template>

<script setup>
import { computed, onBeforeUnmount, onMounted, ref } from "vue";
import Icon from "./Icon.vue";

const props = defineProps({
  modelValue: {
    type: [String, Number, null],
    default: null,
  },
  options: {
    type: Array,
    default: () => [],
  },
  placeholder: {
    type: String,
    default: "Select",
  },
  disabled: {
    type: Boolean,
    default: false,
  },
});

const emit = defineEmits(["update:modelValue", "change"]);

const root = ref(null);
const isOpen = ref(false);

const selectedValue = computed(() => props.modelValue);
const selectedOption = computed(() => {
  return (
    props.options.find((option) => option.value === props.modelValue) || null
  );
});
const selectedLabel = computed(() => {
  return selectedOption.value?.label || props.placeholder;
});

function toggleMenu() {
  if (props.disabled) return;
  isOpen.value = !isOpen.value;
}

function selectOption(value) {
  emit("update:modelValue", value);
  emit("change", value);
  isOpen.value = false;
}

function handleClickOutside(event) {
  if (root.value && !root.value.contains(event.target)) {
    isOpen.value = false;
  }
}

onMounted(() => {
  document.addEventListener("click", handleClickOutside);
});

onBeforeUnmount(() => {
  document.removeEventListener("click", handleClickOutside);
});
</script>
