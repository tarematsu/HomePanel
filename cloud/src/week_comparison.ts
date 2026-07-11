const JST_MS = 9 * 60 * 60 * 1000;
const DAY_MS = 86_400_000;

export interface IsoWeekInfo {
  year: number;
  week: number;
  weekday: number;
}

export interface AlignedWeekComparison {
  current: IsoWeekInfo;
  previousWeek: IsoWeekInfo;
  currentWeekStart: Date;
  currentWeekEnd: Date;
  previousWeekStart: Date;
  previousWeekEnd: Date;
}

function isoWeekInfoForCivilDate(civilDate: Date): IsoWeekInfo {
  const weekday = civilDate.getUTCDay() || 7;
  const thursday = new Date(civilDate.getTime() + (4 - weekday) * DAY_MS);
  const year = thursday.getUTCFullYear();
  const yearStart = new Date(Date.UTC(year, 0, 1));
  const week = Math.ceil(((thursday.getTime() - yearStart.getTime()) / DAY_MS + 1) / 7);
  return { year, week, weekday };
}

export function isoWeekInfoJst(timestampMs: number): IsoWeekInfo {
  const jst = new Date(timestampMs + JST_MS);
  const civilDate = new Date(Date.UTC(jst.getUTCFullYear(), jst.getUTCMonth(), jst.getUTCDate()));
  return isoWeekInfoForCivilDate(civilDate);
}

export function isoWeeksInYear(year: number): number {
  return isoWeekInfoForCivilDate(new Date(Date.UTC(year, 11, 28))).week;
}

export function isoWeekStartJst(year: number, week: number): Date {
  const maximumWeek = isoWeeksInYear(year);
  if (!Number.isInteger(week) || week < 1 || week > maximumWeek) {
    throw new RangeError(`invalid ISO week ${year}-W${week}`);
  }
  const januaryFourth = new Date(Date.UTC(year, 0, 4));
  const januaryFourthWeekday = januaryFourth.getUTCDay() || 7;
  const firstMonday = januaryFourth.getTime() - (januaryFourthWeekday - 1) * DAY_MS;
  return new Date(firstMonday + (week - 1) * 7 * DAY_MS - JST_MS);
}

export function alignedWeekComparison(timestampMs: number): AlignedWeekComparison {
  const current = isoWeekInfoJst(timestampMs);
  const previousWeek = isoWeekInfoJst(timestampMs - 7 * DAY_MS);
  const currentWeekStart = isoWeekStartJst(current.year, current.week);
  const previousWeekStart = new Date(currentWeekStart.getTime() - 7 * DAY_MS);
  return {
    current,
    previousWeek,
    currentWeekStart,
    currentWeekEnd: new Date(currentWeekStart.getTime() + 7 * DAY_MS),
    previousWeekStart,
    previousWeekEnd: currentWeekStart,
  };
}
