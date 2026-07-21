export const iconRegistry = {
  logo: {
    viewBox: "0 0 24 24",
    nodes: [
      {
        tag: "circle",
        attrs: { cx: 12, cy: 12, r: 9.5, "stroke-width": 1.8 },
      },
      {
        tag: "circle",
        attrs: { cx: 12, cy: 12, r: 5.3, "stroke-width": 1.8 },
      },
      {
        tag: "circle",
        attrs: { cx: 12, cy: 12, r: 2.1, "stroke-width": 1.8 },
      },
    ],
  },
  globe: {
    viewBox: "0 0 24 24",
    nodes: [
      {
        tag: "circle",
        attrs: { cx: 12, cy: 12, r: 9.5, "stroke-width": 1.8 },
      },
      {
        tag: "path",
        attrs: { d: "M3 12h18", "stroke-width": 1.8 },
      },
      {
        tag: "path",
        attrs: {
          d: "M12 3a15.5 15.5 0 0 1 4 9 15.5 15.5 0 0 1-4 9 15.5 15.5 0 0 1-4-9 15.5 15.5 0 0 1 4-9Z",
          "stroke-width": 1.8,
        },
      },
    ],
  },
  moon: {
    svg: `<svg xmlns="http://www.w3.org/2000/svg" width="1em" height="1em" viewBox="0 0 24 24">
      <path d="M0 0h24v24H0z" fill="none" />
      <g fill="currentColor">
        <path fill-opacity="0" stroke="currentColor" stroke-dasharray="56" stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M7 6c0 6.08 4.92 11 11 11c0.53 0 1.05 -0.04 1.56 -0.11c-1.61 2.47 -4.39 4.11 -7.56 4.11c-4.97 0 -9 -4.03 -9 -9c0 -3.17 1.64 -5.95 4.11 -7.56c-0.07 0.51 -0.11 1.03 -0.11 1.56Z">
          <animate fill="freeze" attributeName="stroke-dashoffset" dur="0.6s" values="56;0" />
          <animate fill="freeze" attributeName="fill-opacity" begin="0.7s" dur="0.4s" to="1" />
        </path>
        <path d="M15.22 6.03l2.53 -1.94l-3.19 -0.09l-1.06 -3l-1.06 3l-3.19 0.09l2.53 1.94l-0.91 3.06l2.63 -1.81l2.63 1.81l-0.91 -3.06Z" opacity="0">
          <animate fill="freeze" attributeName="opacity" begin="1.2s" dur="0.4s" to="1" />
        </path>
        <path d="M19.61 12.25l1.64 -1.25l-2.06 -0.05l-0.69 -1.95l-0.69 1.95l-2.06 0.05l1.64 1.25l-0.59 1.98l1.7 -1.17l1.7 1.17l-0.59 -1.98Z" opacity="0">
          <animate fill="freeze" attributeName="opacity" begin="1.6s" dur="0.4s" to="1" />
        </path>
      </g>
    </svg>`,
  },
  sun: {
    svg: `<svg xmlns="http://www.w3.org/2000/svg" width="1em" height="1em" viewBox="0 0 24 24">
      <path d="M0 0h24v24H0z" fill="none" />
      <defs>
        <mask id="SVGUXuaaerw">
          <path fill="#fff" d="M12 6c3.31 0 6 2.69 6 6c0 3.31 -2.69 6 -6 6c-3.31 0 -6 -2.69 -6 -6c0 -3.31 2.69 -6 6 -6Z">
            <animate fill="freeze" attributeName="d" dur="0.4s" values="M12 2c5.52 0 10 4.48 10 10c0 5.52 -4.48 10 -10 10c-5.52 0 -10 -4.48 -10 -10c0 -5.52 4.48 -10 10 -10Z;M12 6c3.31 0 6 2.69 6 6c0 3.31 -2.69 6 -6 6c-3.31 0 -6 -2.69 -6 -6c0 -3.31 2.69 -6 6 -6Z" />
          </path>
          <path d="M22 -4c3.31 0 6 2.69 6 6c0 3.31 -2.69 6 -6 6c-3.31 0 -6 -2.69 -6 -6c0 -3.31 2.69 -6 6 -6Z">
            <animate fill="freeze" attributeName="d" dur="0.4s" values="M18 -4c5.52 0 10 4.48 10 10c0 5.52 -4.48 10 -10 10c-5.52 0 -10 -4.48 -10 -10c0 -5.52 4.48 -10 10 -10Z;M22 -4c3.31 0 6 2.69 6 6c0 3.31 -2.69 6 -6 6c-3.31 0 -6 -2.69 -6 -6c0 -3.31 2.69 -6 6 -6Z" />
            <set fill="freeze" attributeName="opacity" begin="0.4s" to="0" />
          </path>
          <path d="M12 8c2.21 0 4 1.79 4 4c0 2.21 -1.79 4 -4 4c-2.21 0 -4 -1.79 -4 -4c0 -2.21 1.79 -4 4 -4Z">
            <animate fill="freeze" attributeName="d" dur="0.4s" values="M12 4c4.42 0 8 3.58 8 8c0 4.42 -3.58 8 -8 8c-4.42 0 -8 -3.58 -8 -8c0 -4.42 3.58 -8 8 -8Z;M12 8c2.21 0 4 1.79 4 4c0 2.21 -1.79 4 -4 4c-2.21 0 -4 -1.79 -4 -4c0 -2.21 1.79 -4 4 -4Z" />
          </path>
        </mask>
        <mask id="SVGjqg97bFZ">
          <path fill="#fff" d="M22 -6c4.42 0 8 3.58 8 8c0 4.42 -3.58 8 -8 8c-4.42 0 -8 -3.58 -8 -8c0 -4.42 3.58 -8 8 -8Z">
            <animate fill="freeze" attributeName="d" dur="0.4s" values="M18 -6c6.62 0 12 5.38 12 12c0 6.62 -5.38 12 -12 12c-6.62 0 -12 -5.38 -12 -12c0 -6.62 5.38 -12 12 -12Z;M22 -6c4.42 0 8 3.58 8 8c0 4.42 -3.58 8 -8 8c-4.42 0 -8 -3.58 -8 -8c0 -4.42 3.58 -8 8 -8Z" />
            <set fill="freeze" attributeName="opacity" begin="0.4s" to="0" />
          </path>
          <path d="M22 -4c3.31 0 6 2.69 6 6c0 3.31 -2.69 6 -6 6c-3.31 0 -6 -2.69 -6 -6c0 -3.31 2.69 -6 6 -6ZM6 12v-20h26v26h-20c3.31 0 6 -2.69 6 -6c0 -3.31 -2.69 -6 -6 -6c-3.31 0 -6 2.69 -6 6Z">
            <animate fill="freeze" attributeName="d" dur="0.4s" values="M18 -4c5.52 0 10 4.48 10 10c0 5.52 -4.48 10 -10 10c-5.52 0 -10 -4.48 -10 -10c0 -5.52 4.48 -10 10 -10ZM2 12v-20h30v30h-20c5.52 0 10 -4.48 10 -10c0 -5.52 -4.48 -10 -10 -10c-5.52 0 -10 4.48 -10 10Z;M22 -4c3.31 0 6 2.69 6 6c0 3.31 -2.69 6 -6 6c-3.31 0 -6 -2.69 -6 -6c0 -3.31 2.69 -6 6 -6ZM6 12v-20h26v26h-20c3.31 0 6 -2.69 6 -6c0 -3.31 -2.69 -6 -6 -6c-3.31 0 -6 2.69 -6 6Z" />
            <set fill="freeze" attributeName="opacity" begin="0.4s" to="0" />
          </path>
        </mask>
      </defs>
      <g fill="currentColor">
        <path d="M0 0h24v24H0z" mask="url(#SVGUXuaaerw)" />
        <path d="M0 0h24v24H0z" mask="url(#SVGjqg97bFZ)" />
      </g>
      <g fill="none" stroke="currentColor" stroke-linecap="round" stroke-linejoin="round" stroke-width="2">
        <path d="M12 21v1M21 12h1M12 3v-1M3 12h-1" opacity="0">
          <animateTransform attributeName="transform" dur="30s" repeatCount="indefinite" type="rotate" values="0 12 12;360 12 12" />
          <set fill="freeze" attributeName="opacity" begin="0.4s" to="1" />
          <animate fill="freeze" attributeName="d" begin="0.4s" dur="0.2s" values="M12 19v1M19 12h1M12 5v-1M5 12h-1;M12 21v1M21 12h1M12 3v-1M3 12h-1" />
        </path>
        <path d="M18.5 18.5l0.5 0.5M18.5 5.5l0.5 -0.5M5.5 5.5l-0.5 -0.5M5.5 18.5l-0.5 0.5" opacity="0">
          <animateTransform attributeName="transform" dur="30s" repeatCount="indefinite" type="rotate" values="0 12 12;360 12 12" />
          <set fill="freeze" attributeName="opacity" begin="0.6s" to="1" />
          <animate fill="freeze" attributeName="d" begin="0.6s" dur="0.2s" values="M17 17l0.5 0.5M17 7l0.5 -0.5M7 7l-0.5 -0.5M7 17l-0.5 0.5;M18.5 18.5l0.5 0.5M18.5 5.5l0.5 -0.5M5.5 5.5l-0.5 -0.5M5.5 18.5l-0.5 0.5" />
        </path>
      </g>
    </svg>`,
  },
  systemTheme: {
    svg: `<svg xmlns="http://www.w3.org/2000/svg" width="1em" height="1em" viewBox="0 0 24 24">
      <path d="M0 0h24v24H0z" fill="none" />
      <defs>
        <mask id="SVGPWNvGc7L">
          <circle cx="7.5" cy="7.5" r="5.5" fill="#fff" />
          <circle cx="11" cy="7.5" r="6.5">
            <animate fill="freeze" attributeName="cx" dur="0.4s" values="7.5;11" />
            <animate fill="freeze" attributeName="r" dur="0.4s" values="5.5;6.5" />
          </circle>
        </mask>
        <mask id="SVGE5iORxWn">
          <g fill="#fff">
            <circle cx="12" cy="9" r="5.5" transform="rotate(-45 12 12)">
              <animate fill="freeze" attributeName="cy" begin="0.9s" dur="0.5s" to="15" />
            </circle>
            <path d="M12.62 20.62h3l-1.5 2.5Z" opacity="0">
              <animateTransform attributeName="transform" dur="5s" repeatCount="indefinite" type="rotate" values="-145 14.12 14.12;-95 14.12 14.12" />
              <set fill="freeze" attributeName="opacity" begin="1.4s" to="1" />
              <animate fill="freeze" attributeName="d" begin="1.4s" dur="0.4s" values="M13.12 17.12h2l-1 2Z;M12.62 20.62h3l-1.5 2.5Z" />
            </path>
            <path d="M12.62 20.62h3l-1.5 2.5Z" opacity="0">
              <animateTransform attributeName="transform" dur="5s" repeatCount="indefinite" type="rotate" values="-95 14.12 14.12;-45 14.12 14.12" />
              <set fill="freeze" attributeName="opacity" begin="1.4s" to="1" />
              <animate fill="freeze" attributeName="d" begin="1.4s" dur="0.4s" values="M13.12 17.12h2l-1 2Z;M12.62 20.62h3l-1.5 2.5Z" />
            </path>
            <path d="M12.62 20.62h3l-1.5 2.5Z" opacity="0">
              <animateTransform attributeName="transform" dur="5s" repeatCount="indefinite" type="rotate" values="-45 14.12 14.12;5 14.12 14.12" />
              <set fill="freeze" attributeName="opacity" begin="1.4s" to="1" />
              <animate fill="freeze" attributeName="d" begin="1.4s" dur="0.4s" values="M13.12 17.12h2l-1 2Z;M12.62 20.62h3l-1.5 2.5Z" />
            </path>
            <path d="M12.62 20.62h3l-1.5 2.5Z" opacity="0">
              <animateTransform attributeName="transform" dur="5s" repeatCount="indefinite" type="rotate" values="5 14.12 14.12;55 14.12 14.12" />
              <set fill="freeze" attributeName="opacity" begin="1.4s" to="1" />
              <animate fill="freeze" attributeName="d" begin="1.4s" dur="0.4s" values="M13.12 17.12h2l-1 2Z;M12.62 20.62h3l-1.5 2.5Z" />
            </path>
          </g>
          <path d="M-4.97 12l18.38 -18.38l10.61 10.61l-18.38 18.38Z" />
        </mask>
      </defs>
      <g fill="currentColor">
        <path d="M0 0h24v24H0z" mask="url(#SVGPWNvGc7L)" />
        <path d="M0 0h24v24H0z" mask="url(#SVGE5iORxWn)" />
      </g>
      <path fill="none" stroke="currentColor" stroke-dasharray="24" stroke-dashoffset="24" stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M22 12h-22" transform="rotate(-45 12 12)">
        <animate attributeName="d" dur="5s" keyTimes="0;0.5;1" repeatCount="indefinite" values="M22 12h-22;M24 12h-22;M22 12h-22" />
        <animate fill="freeze" attributeName="stroke-dashoffset" begin="0.5s" dur="0.3s" to="0" />
      </path>
    </svg>`,
  },
  github: {
    viewBox: "0 0 24 24",
    nodes: [
      {
        tag: "path",
        attrs: {
          d: "M12 0C5.37 0 0 5.37 0 12c0 5.31 3.43 9.8 8.2 11.38.6.11.82-.26.82-.57 0-.28-.01-1.23-.01-2.23-3.01.56-3.65-.72-3.88-1.39-.13-.34-.7-1.39-1.2-1.67-.41-.22-1-.77-.01-.78.95-.01 1.63.87 1.86 1.24 1.09 1.84 2.83 1.32 3.53.99.11-.78.43-1.31.77-1.61-2.69-.31-5.51-1.35-5.51-5.97 0-1.32.47-2.4 1.24-3.25-.12-.31-.54-1.54.12-3.2 0 0 1.01-.32 3.31 1.23.96-.27 1.99-.4 3.01-.4 1.02 0 2.05.13 3.01.4 2.3-1.55 3.31-1.23 3.31-1.23.66 1.66.24 2.89.12 3.2.77.85 1.24 1.93 1.24 3.25 0 4.63-2.83 5.66-5.53 5.96.43.38.81 1.11.81 2.24 0 1.62-.01 2.92-.01 3.32 0 .31.22.69.83.57A12.01 12.01 0 0 0 24 12C24 5.37 18.63 0 12 0Z",
          fill: "currentColor",
        },
      },
    ],
  },
  check: {
    viewBox: "0 0 24 24",
    nodes: [
      {
        tag: "path",
        attrs: { d: "M20 6 9 17l-5-5", "stroke-width": 2.2 },
      },
    ],
  },
  chevronDown: {
    viewBox: "0 0 24 24",
    nodes: [
      {
        tag: "path",
        attrs: { d: "m6 9 6 6 6-6", "stroke-width": 2 },
      },
    ],
  },
  chevronRight: {
    viewBox: "0 0 24 24",
    nodes: [
      {
        tag: "path",
        attrs: { d: "m9 18 6-6-6-6", "stroke-width": 2 },
      },
    ],
  },
  status: {
    viewBox: "0 0 24 24",
    nodes: [
      { tag: "path", attrs: { d: "M4 19h16", "stroke-width": 1.8 } },
      { tag: "path", attrs: { d: "M7 15v-4", "stroke-width": 1.8 } },
      { tag: "path", attrs: { d: "M12 15V8", "stroke-width": 1.8 } },
      { tag: "path", attrs: { d: "M17 15v-2", "stroke-width": 1.8 } },
    ],
  },
  settings: {
    viewBox: "0 0 24 24",
    nodes: [
      {
        tag: "circle",
        attrs: { cx: 12, cy: 12, r: 3.8, "stroke-width": 1.8 },
      },
      {
        tag: "path",
        attrs: {
          d: "M12 2.75v2.5M12 18.75v2.5M3.75 12h2.5M17.75 12h2.5M5.5 5.5l1.8 1.8M16.7 16.7l1.8 1.8M5.5 18.5l1.8-1.8M16.7 7.3l1.8-1.8",
          "stroke-width": 1.8,
        },
      },
    ],
  },
  refresh: {
    viewBox: "0 0 24 24",
    nodes: [
      {
        tag: "path",
        attrs: {
          d: "M21 12a9 9 0 0 1-15 6.4M3 12a9 9 0 0 1 15-6.4M15 4h5v5",
          "stroke-width": 1.8,
        },
      },
    ],
  },
  network: {
    viewBox: "0 0 24 24",
    nodes: [
      {
        tag: "circle",
        attrs: { cx: 6, cy: 12, r: 2.2, "stroke-width": 1.8 },
      },
      {
        tag: "circle",
        attrs: { cx: 12, cy: 6, r: 2.2, "stroke-width": 1.8 },
      },
      {
        tag: "circle",
        attrs: { cx: 12, cy: 18, r: 2.2, "stroke-width": 1.8 },
      },
      {
        tag: "circle",
        attrs: { cx: 18, cy: 12, r: 2.2, "stroke-width": 1.8 },
      },
      {
        tag: "path",
        attrs: {
          d: "M8.2 10.6 10.7 8.2M13.3 8.2l2.5 2.4M8.2 13.4l2.5 2.4M13.3 15.8l2.5-2.4",
          "stroke-width": 1.8,
        },
      },
    ],
  },
  logs: {
    viewBox: "0 0 24 24",
    nodes: [
      {
        tag: "path",
        attrs: {
          d: "M7 4h10a2 2 0 0 1 2 2v12a2 2 0 0 1-2 2H7",
          "stroke-width": 1.8,
        },
      },
      { tag: "path", attrs: { d: "M5 4h2v16H5", "stroke-width": 1.8 } },
      { tag: "path", attrs: { d: "M8 8h8", "stroke-width": 1.8 } },
      { tag: "path", attrs: { d: "M8 12h8", "stroke-width": 1.8 } },
      { tag: "path", attrs: { d: "M8 16h5", "stroke-width": 1.8 } },
    ],
  },
  tools: {
    viewBox: "0 0 24 24",
    nodes: [
      {
        tag: "path",
        attrs: {
          d: "M14.7 6.3a4 4 0 0 0-5.4 5.4l-6 6a2 2 0 0 0 2.8 2.8l6-6a4 4 0 0 0 5.4-5.4l-2.5 2.5-2.1-2.1 2.5-2.5Z",
          "stroke-width": 1.8,
          "stroke-linejoin": "round",
          "stroke-linecap": "round",
        },
      },
    ],
  },
  desktop: {
    viewBox: "0 0 24 24",
    nodes: [
      {
        tag: "rect",
        attrs: {
          x: 3,
          y: 4,
          width: 18,
          height: 12,
          rx: 2,
          "stroke-width": 1.8,
        },
      },
      {
        tag: "line",
        attrs: { x1: 8, y1: 20, x2: 16, y2: 20, "stroke-width": 1.8 },
      },
      {
        tag: "line",
        attrs: { x1: 12, y1: 16, x2: 12, y2: 20, "stroke-width": 1.8 },
      },
    ],
  },
  sliders: {
    viewBox: "0 0 24 24",
    nodes: [
      { tag: "line", attrs: { x1: 4, y1: 21, x2: 4, y2: 14, "stroke-width": 1.8 } },
      { tag: "line", attrs: { x1: 4, y1: 10, x2: 4, y2: 3, "stroke-width": 1.8 } },
      { tag: "line", attrs: { x1: 12, y1: 21, x2: 12, y2: 12, "stroke-width": 1.8 } },
      { tag: "line", attrs: { x1: 12, y1: 8, x2: 12, y2: 3, "stroke-width": 1.8 } },
      { tag: "line", attrs: { x1: 20, y1: 21, x2: 20, y2: 16, "stroke-width": 1.8 } },
      { tag: "line", attrs: { x1: 20, y1: 12, x2: 20, y2: 3, "stroke-width": 1.8 } },
      { tag: "line", attrs: { x1: 1, y1: 14, x2: 7, y2: 14, "stroke-width": 1.8 } },
      { tag: "line", attrs: { x1: 9, y1: 8, x2: 15, y2: 8, "stroke-width": 1.8 } },
      { tag: "line", attrs: { x1: 17, y1: 16, x2: 23, y2: 16, "stroke-width": 1.8 } },
    ],
  },
  close: {
    viewBox: "0 0 24 24",
    nodes: [
      { tag: "line", attrs: { x1: 6, y1: 6, x2: 18, y2: 18, "stroke-width": 1.8 } },
      { tag: "line", attrs: { x1: 18, y1: 6, x2: 6, y2: 18, "stroke-width": 1.8 } },
    ],
  },
  save: {
    viewBox: "0 0 24 24",
    nodes: [
      {
        tag: "path",
        attrs: {
          d: "M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z",
        },
      },
      { tag: "path", attrs: { d: "M17 21v-8H7v8" } },
      { tag: "path", attrs: { d: "M7 3v5h8V3" } },
    ],
  },
  tabs: {
    viewBox: "0 0 24 24",
    nodes: [
      { tag: "rect", attrs: { x: 3, y: 9, width: 18, height: 11, rx: 2, "stroke-width": 1.8 } },
      {
        tag: "path",
        attrs: { d: "M7 9V6.5A1.5 1.5 0 0 1 8.5 5H11l1.5 2", "stroke-width": 1.8 },
      },
    ],
  },
};
