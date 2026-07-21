import { describe, expect, it } from "vitest";
import {
  buildParameterizedInsert,
  columnNamesFromPragma,
  oversizedSqlStatements,
  quoteSqliteIdentifier,
} from "../scripts/d1-import-utils.mjs";

describe("D1 import helpers", () => {
  it("detects statements above the configured UTF-8 byte limit", () => {
    const sql = `PRAGMA defer_foreign_keys=TRUE;\nINSERT INTO t VALUES('${"あ".repeat(40)}');`;
    expect(oversizedSqlStatements(sql, 100)).toEqual([{ line: 2, bytes: 145 }]);
  });

  it("quotes SQLite identifiers", () => {
    expect(quoteSqliteIdentifier('capture"table')).toBe('"capture""table"');
  });

  it("keeps PRAGMA column order stable", () => {
    expect(columnNamesFromPragma([
      { cid: 2, name: "note" },
      { cid: 0, name: "id" },
      { cid: 1, name: "captured_at" },
    ])).toEqual(["id", "captured_at", "note"]);
  });

  it("builds a small parameterized insert while preserving nulls", () => {
    expect(buildParameterizedInsert("snapshots", ["id", "html_text", "note"], {
      id: 7,
      html_text: "x".repeat(250_000),
      note: null,
    })).toEqual({
      sql: 'INSERT INTO "snapshots" ("id","html_text","note") VALUES(?,?,NULL);',
      params: ["7", "x".repeat(250_000)],
    });
  });
});
