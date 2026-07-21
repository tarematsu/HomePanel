export const D1_SAFE_STATEMENT_BYTES = 90_000;

const utf8 = new TextEncoder();

export function oversizedSqlStatements(sql, limitBytes = D1_SAFE_STATEMENT_BYTES) {
  if (!Number.isSafeInteger(limitBytes) || limitBytes <= 0) {
    throw new TypeError('limitBytes must be a positive safe integer');
  }

  const oversized = [];
  const lines = String(sql).split(/\r?\n/);
  for (let index = 0; index < lines.length; index += 1) {
    const bytes = utf8.encode(lines[index]).byteLength;
    if (bytes > limitBytes) oversized.push({ line: index + 1, bytes });
  }
  return oversized;
}

export function quoteSqliteIdentifier(value) {
  const identifier = String(value || '');
  if (!identifier || identifier.includes('\0')) {
    throw new TypeError('SQLite identifier must be non-empty and cannot contain NUL');
  }
  return `"${identifier.replaceAll('"', '""')}"`;
}

export function columnNamesFromPragma(rows) {
  if (!Array.isArray(rows) || rows.length === 0) {
    throw new Error('PRAGMA table_info returned no columns');
  }

  const columns = [...rows]
    .sort((left, right) => Number(left?.cid) - Number(right?.cid))
    .map((row) => String(row?.name || ''));

  if (columns.some((name) => !name)) throw new Error('PRAGMA table_info returned an unnamed column');
  if (new Set(columns).size !== columns.length) throw new Error('PRAGMA table_info returned duplicate columns');
  return columns;
}

function normalizeD1Parameter(value, column) {
  if (typeof value === 'string') return value;
  if (typeof value === 'number' && Number.isFinite(value)) return String(value);
  if (typeof value === 'bigint') return value.toString();
  if (typeof value === 'boolean') return value ? '1' : '0';
  throw new TypeError(`Unsupported D1 parameter type for ${column}: ${typeof value}`);
}

export function buildParameterizedInsert(table, columns, row) {
  if (!Array.isArray(columns) || columns.length === 0) {
    throw new TypeError('columns must be a non-empty array');
  }
  if (!row || typeof row !== 'object' || Array.isArray(row)) {
    throw new TypeError('row must be an object');
  }

  const params = [];
  const values = columns.map((column) => {
    if (!Object.hasOwn(row, column)) throw new Error(`Source row is missing column: ${column}`);
    const value = row[column];
    if (value === null || value === undefined) return 'NULL';
    params.push(normalizeD1Parameter(value, column));
    return '?';
  });

  const sql = `INSERT INTO ${quoteSqliteIdentifier(table)} (${columns.map(quoteSqliteIdentifier).join(',')}) VALUES(${values.join(',')});`;
  return { sql, params };
}
