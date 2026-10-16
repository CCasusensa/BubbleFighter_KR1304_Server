/**
 * 宠物技能：一只宠物只有两条技能。
 *
 * 客户端宠物窗就是两行，「2 等级」一条、「3 等级」一条（服务端
 * internal/game/pets_session.go 的 skill-slot trio 自己维护，客户端不读）。
 * 数据库里存的是 `attrs` —— 一档一行 `{"a": 技能行号, "grade": 1|2}`；页面把
 * 它摊成两个下拉（pick1 / pick2），保存时再折回 attrs。
 *
 * 运营者 2026-10-12：宠物页那三个「技能 1/2/3」既不是客户端看的字段、也没有
 * 映射成技能名，所以这里把「两档 + 名字」钉死在一个文件里，页面只管画。
 */
export type Row = Record<string, any>

/** 宠物身上那一档的当前技能行号（没有 = 0）。 */
export function petGradePick(pet: Row, grade: number): number {
  const entry = (pet?.attrs || []).find((row: Row) => Number(row?.grade ?? row?.b) === grade)
  const index = Number(entry?.a || 0)
  return Number.isFinite(index) && index > 0 ? index : 0
}

/** 快照里的每只宠物补上 pick1 / pick2，给两个下拉当 v-model。 */
export function withPetPicks(row: Row): Row {
  for (const pet of Object.values(row?.pets || {}) as Row[]) {
    if (pet.pick1 === undefined) pet.pick1 = petGradePick(pet, 1)
    if (pet.pick2 === undefined) pet.pick2 = petGradePick(pet, 2)
  }
  return row
}

/** 两个下拉 -> 提交的 attrs：只有 grade 1 / grade 2，空档不写。 */
export function petAttrsFromPicks(pet: Row): Row[] {
  const out: Row[] = []
  if (Number(pet?.pick1) > 0) out.push({ a: Number(pet.pick1), grade: 1 })
  if (Number(pet?.pick2) > 0) out.push({ a: Number(pet.pick2), grade: 2 })
  return out
}

/** 两个下拉的可选技能：按档取池子里没退役、也不是状态行的那些。 */
export function petSkillOptions(abilities: unknown, grade: number): Row[] {
  return (Array.isArray(abilities) ? abilities : []).filter((row: Row) =>
    Number(row?.grade) === grade && !row?.state && !row?.excluded)
}
