import { describe, expect, it } from "vitest";
import { ago, formatBytes, formatValue, isUp, toColumns } from "./format";

describe("format", () => {
  it("converts points to uPlot columns", () => {
    expect(toColumns([{ t: 1, avg: 2, min: 1, max: 3 }, { t: 2, avg: 4, min: 3, max: 5 }])).toEqual([
      [1, 2],
      [2, 4],
      [1, 3],
      [3, 5],
    ]);
    expect(toColumns([])).toEqual([[], [], [], []]);
  });

  it("formats bytes", () => {
    expect(formatBytes(0)).toBe("0 B");
    expect(formatBytes(1536)).toBe("1.5 KB");
    expect(formatBytes(8 * 1024 ** 3)).toBe("8.0 GB");
  });

  it("formats metric values by unit", () => {
    expect(formatValue("cpu.util_total", 23.46)).toBe("23.5%");
    expect(formatValue("gpu.temp_c", 64.2)).toBe("64 °C");
    expect(formatValue("gpu.power_w", 182.44)).toBe("182.4 W");
    expect(formatValue("net.rx_bytes_per_s", 2048)).toBe("2.0 KB/s");
    expect(formatValue("gpu.util", undefined)).toBe("–");
    expect(formatValue("gpu.util", NaN)).toBe("–");
  });

  it("decides host liveness at the 60 s boundary", () => {
    expect(isUp(100, 160)).toBe(true);
    expect(isUp(100, 161)).toBe(false);
  });

  it("formats ages", () => {
    expect(ago(5)).toBe("5s ago");
    expect(ago(600)).toBe("10m ago");
    expect(ago(7200)).toBe("2h ago");
  });
});

import { mergeSeries } from "./format";

describe("mergeSeries", () => {
  it("aligns series on the union of timestamps with null gaps", () => {
    const a = { label: "a", points: [{ t: 1, avg: 10, min: 10, max: 10 }, { t: 3, avg: 30, min: 30, max: 30 }] };
    const b = { label: "b", points: [{ t: 2, avg: 20, min: 20, max: 20 }] };
    expect(mergeSeries([a, b])).toEqual([[1, 2, 3], [10, null, 30], [null, 20, null]]);
    expect(mergeSeries([])).toEqual([[]]);
  });
});
