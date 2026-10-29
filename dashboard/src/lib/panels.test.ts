import { describe, expect, it } from "vitest";
import { panelsFor } from "./panels";

describe("panelsFor", () => {
  it("builds one panel per known metric with a series per device", () => {
    const panels = panelsFor([
      { metric: "cpu.util_total", device: "total" },
      { metric: "cpu.util_core", device: "0" },
      { metric: "cpu.util_core", device: "1" },
      { metric: "gpu.util", device: "gpu0" },
      { metric: "unknown.metric", device: "" },
    ]);
    expect(panels.map((p) => p.metric)).toEqual(["cpu.util_total", "cpu.util_core", "gpu.util"]);
    expect(panels[1].devices).toEqual(["0", "1"]);
  });

  it("skips total-only specs when the device is missing", () => {
    expect(panelsFor([{ metric: "cpu.util_total", device: "other" }])).toEqual([]);
    expect(panelsFor([])).toEqual([]);
  });
});
