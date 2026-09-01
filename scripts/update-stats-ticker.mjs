#!/usr/bin/env node
// Generates assets/stats-ticker.svg: a split-flap (airport departure board)
// style widget showing cumulative-all-time views, total release downloads,
// and GitHub stars, in the MelonDMA brand palette. Each digit tile "flips"
// open once when the image loads (no continuous scrolling).
//
// GitHub's traffic API only exposes a rolling 14-day view window, not an
// all-time total, so assets/stats-state.json banks each day's count exactly
// once (the day it first ages out of "today") into a running total that
// only grows. Today's own count is re-read fresh every run and added on
// top, so the number is live intraday without ever double-counting a
// finalized day.

import { existsSync, readFileSync, writeFileSync } from "node:fs";

const owner = process.env.GITHUB_REPOSITORY_OWNER || "denmrnngp-cloud";
const repo = (process.env.GITHUB_REPOSITORY || `${owner}/MelonDMA`).split("/")[1];
const tickerPath = "assets/stats-ticker.svg";
const token = process.env.GITHUB_TOKEN || "";
const headers = {
  Accept: "application/vnd.github+json",
  "X-GitHub-Api-Version": "2022-11-28",
  ...(token ? { Authorization: `Bearer ${token}` } : {}),
};

async function github(path, fallback) {
  try {
    const response = await fetch(`https://api.github.com/repos/${owner}/${repo}${path}`, { headers });
    if (!response.ok) throw new Error(`${path}: ${response.status} ${response.statusText}`);
    return await response.json();
  } catch (error) {
    console.warn(`warning: ${error.message}`);
    return fallback;
  }
}

async function githubPages(path) {
  const items = [];
  for (let page = 1; page <= 10; page += 1) {
    const separator = path.includes("?") ? "&" : "?";
    const batch = await github(`${path}${separator}page=${page}`, []);
    if (!Array.isArray(batch)) break;
    items.push(...batch);
    if (batch.length < 100) break;
  }
  return items;
}

function xml(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
}

function readJson(path, fallback) {
  if (!existsSync(path)) return fallback;
  try {
    return JSON.parse(readFileSync(path, "utf8"));
  } catch (error) {
    console.warn(`warning: ${path}: ${error.message}`);
    return fallback;
  }
}

const statePath = "assets/stats-state.json";

const repoInfo = await github("", { stargazers_count: 0 });
const viewsResp = await github("/traffic/views?per=day", { views: [] });
const releases = await githubPages("/releases?per_page=100");

let totalDownloads = 0;
for (const release of releases) {
  for (const asset of release.assets || []) {
    totalDownloads += asset.download_count || 0;
  }
}

// Bank each day's view count exactly once, the first time we see it as a
// day strictly before "today" (so a day's count is only ever added after
// it can no longer change). Today's count is re-read fresh every run.
const state = readJson(statePath, { countedDays: {}, finalizedViews: 0 });
const todayKey = new Date().toISOString().slice(0, 10);
let todayViews = 0;
for (const day of viewsResp.views || []) {
  const dateKey = String(day.timestamp).slice(0, 10);
  if (dateKey === todayKey) {
    todayViews = day.count || 0;
  } else if (dateKey < todayKey && !state.countedDays[dateKey]) {
    state.finalizedViews += day.count || 0;
    state.countedDays[dateKey] = true;
  }
}
// Keep the counted-days ledger from growing forever (14-day API window
// means anything older than ~30 days can never be re-seen).
const cutoff = new Date(Date.now() - 30 * 24 * 60 * 60 * 1000).toISOString().slice(0, 10);
for (const dateKey of Object.keys(state.countedDays)) {
  if (dateKey < cutoff) delete state.countedDays[dateKey];
}
writeFileSync(statePath, `${JSON.stringify(state, null, 2)}\n`);

const stars = repoInfo.stargazers_count || 0;
const viewCount = state.finalizedViews + todayViews;

// --- layout constants (brand: warm melon rind palette) ---
// Fixed total width: matches the logo's display width in README.md
// (<img src="MelonDMA.png" width="640">), per an explicit ask that the
// board be exactly as wide as the logo. Tile width is derived from this
// budget so the board stays 640px wide even if a count grows a digit.
const TOTAL_WIDTH = 640;
const SIDE_PAD = 30;
const GROUP_GAP = 56;
const TILE_GAP = 8;

const COLORS = {
  boardFrom: "#241608",
  boardMid: "#1a0f06",
  border: "#C97A1E",
  tileTop: "#2f1e0e",
  tileBottom: "#190f06",
  flap: "#3a2410",
  seam: "#0f0904",
  digit: "#FFD27A",
  label: "#D9A566",
  fine: "#8B7860",
  live: "#5FA347",
};

function padded(value, minDigits) {
  return String(Math.max(0, Math.trunc(value))).padStart(minDigits, "0");
}

const groups = [
  { label: "VIEWS", digits: padded(viewCount, 3) },
  { label: "DOWNLOADS", digits: padded(totalDownloads, 2) },
  { label: "STARS", digits: padded(stars, 2) },
];

// Derive tile width from the fixed 640px budget so the board always spans
// exactly TOTAL_WIDTH, even if a count grows an extra digit later.
const totalDigits = groups.reduce((sum, g) => sum + g.digits.length, 0);
const innerGaps = totalDigits - groups.length;
const availableForTiles = TOTAL_WIDTH - 2 * SIDE_PAD - (groups.length - 1) * GROUP_GAP - innerGaps * TILE_GAP;
const TILE_W = availableForTiles / totalDigits;
const TILE_H = Math.round(TILE_W * 1.42);
const DIGIT_FONT = Math.round(TILE_H * 0.56);
const LABEL_FONT = 15;
const TOP_PAD = 20;
const LABEL_Y = TOP_PAD + LABEL_FONT;
const TILES_Y = LABEL_Y + 14;
const FINE_Y = TILES_Y + TILE_H + 28;
const HEIGHT = FINE_Y + 16;

let globalDigitIndex = 0;
let cursorX = SIDE_PAD;
const groupSvgParts = [];
const separatorSvgParts = [];

groups.forEach((group, groupIndex) => {
  const tilesWidth = group.digits.length * TILE_W + (group.digits.length - 1) * TILE_GAP;
  const groupX = cursorX;
  const groupCenterX = groupX + tilesWidth / 2;

  groupSvgParts.push(
    `<text x="${groupCenterX.toFixed(1)}" y="${LABEL_Y}" text-anchor="middle" font-family="Arial, Helvetica, sans-serif" font-weight="700" font-size="${LABEL_FONT}" letter-spacing="3" fill="${COLORS.label}">${xml(group.label)}</text>`
  );

  [...group.digits].forEach((digit, i) => {
    const tileX = groupX + i * (TILE_W + TILE_GAP);
    const delay = (0.15 + globalDigitIndex * 0.09).toFixed(2);
    globalDigitIndex += 1;
    groupSvgParts.push(`
    <g transform="translate(${tileX.toFixed(1)},${TILES_Y})">
      <rect width="${TILE_W.toFixed(1)}" height="${TILE_H}" rx="10" fill="url(#tileBg)" stroke="${COLORS.border}" stroke-opacity="0.25"/>
      <line x1="3" y1="${TILE_H / 2}" x2="${(TILE_W - 3).toFixed(1)}" y2="${TILE_H / 2}" stroke="${COLORS.seam}" stroke-width="1.5" opacity="0.85"/>
      <text x="${(TILE_W / 2).toFixed(1)}" y="${TILE_H / 2 + DIGIT_FONT * 0.36}" text-anchor="middle" font-family="Arial, Helvetica, sans-serif" font-weight="900" font-size="${DIGIT_FONT}" fill="${COLORS.digit}">${digit}</text>
      <rect x="0" y="${TILE_H / 2}" width="${TILE_W.toFixed(1)}" height="0" rx="10" fill="${COLORS.flap}">
        <animate attributeName="height" values="0;${TILE_H};0" keyTimes="0;0.55;1" dur="0.7s" begin="${delay}s" calcMode="spline" keySplines="0.3 0.7 0.3 1;0.3 0 0.7 1"/>
        <animate attributeName="y" values="${TILE_H / 2};0;${TILE_H / 2}" keyTimes="0;0.55;1" dur="0.7s" begin="${delay}s" calcMode="spline" keySplines="0.3 0.7 0.3 1;0.3 0 0.7 1"/>
      </rect>
    </g>`);
  });

  cursorX = groupX + tilesWidth + GROUP_GAP;

  if (groupIndex < groups.length - 1) {
    const sepX = groupX + tilesWidth + GROUP_GAP / 2;
    separatorSvgParts.push(
      `<text x="${sepX.toFixed(1)}" y="${TILES_Y + TILE_H / 2 + 6}" text-anchor="middle" font-family="Arial, Helvetica, sans-serif" font-size="22" fill="${COLORS.label}" opacity="0.5">&#8226;</text>`
    );
  }
});

const width = TOTAL_WIDTH;
const now = new Date();
const updated = now.toISOString().slice(0, 16).replace("T", " ") + " UTC";
const ariaLabel = `MelonDMA live stats — total views: ${viewCount} (tracked since ${Object.keys(state.countedDays).sort()[0] || todayKey}, GitHub only retains 14 days per API call so earlier history can't be recovered), total downloads: ${totalDownloads}, GitHub stars: ${stars}. Updated ${updated}.`;

const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="${width}" height="${HEIGHT}" viewBox="0 0 ${width} ${HEIGHT}" role="img" aria-label="${xml(ariaLabel)}">
  <defs>
    <linearGradient id="boardBg" x1="0" y1="0" x2="${width}" y2="0" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="${COLORS.boardFrom}"/>
      <stop offset="0.5" stop-color="${COLORS.boardMid}"/>
      <stop offset="1" stop-color="${COLORS.boardFrom}"/>
    </linearGradient>
    <linearGradient id="tileBg" x1="0" y1="0" x2="0" y2="${TILE_H}" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="${COLORS.tileTop}"/>
      <stop offset="1" stop-color="${COLORS.tileBottom}"/>
    </linearGradient>
  </defs>
  <rect x="0.5" y="0.5" width="${width - 1}" height="${HEIGHT - 1}" rx="18" fill="url(#boardBg)" stroke="${COLORS.border}" stroke-opacity="0.35"/>
  ${groupSvgParts.join("\n  ")}
  ${separatorSvgParts.join("\n  ")}
  <circle cx="${SIDE_PAD}" cy="${HEIGHT - 16}" r="5" fill="${COLORS.live}">
    <animate attributeName="opacity" values="1;0.35;1" dur="1.8s" repeatCount="indefinite"/>
  </circle>
  <text x="${SIDE_PAD + 12}" y="${HEIGHT - 12}" font-family="Arial, Helvetica, sans-serif" font-size="11" fill="${COLORS.fine}">auto-updates every few minutes &#8226; updated ${xml(updated)}</text>
</svg>
`;

writeFileSync(tickerPath, svg);
console.log(`updated ${tickerPath}: views=${viewCount} downloads=${totalDownloads} stars=${stars}`);
