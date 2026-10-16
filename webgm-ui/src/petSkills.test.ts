import { describe, expect, it } from 'vitest'
import { petAttrsFromPicks, petGradePick, petSkillOptions, withPetPicks } from './petSkills'

describe('pet skill picks', () => {
  it('reads each grade out of the stored attrs rows', () => {
    const pet = { attrs: [{ a: 45, grade: 1 }, { a: 3, grade: 2 }] }
    expect(petGradePick(pet, 1)).toBe(45)
    expect(petGradePick(pet, 2)).toBe(3)
  })

  it('falls back to the row\'s own b field and to 0 when absent', () => {
    expect(petGradePick({ attrs: [{ a: 71, b: 2 }] }, 2)).toBe(71)
    expect(petGradePick({ attrs: [] }, 1)).toBe(0)
    expect(petGradePick({}, 1)).toBe(0)
  })

  it('decorates a snapshot without clobbering picks already set', () => {
    const row = withPetPicks({
      pets: { '301014': { attrs: [{ a: 45, grade: 1 }] }, '301015': { pick2: 9, attrs: [] } },
    })
    expect(row.pets['301014'].pick1).toBe(45)
    expect(row.pets['301014'].pick2).toBe(0)
    expect(row.pets['301015'].pick2).toBe(9)
  })

  it('folds the two picks back into attrs and drops empty grades', () => {
    expect(petAttrsFromPicks({ pick1: 45, pick2: 3 })).toEqual([
      { a: 45, grade: 1 }, { a: 3, grade: 2 },
    ])
    expect(petAttrsFromPicks({ pick1: 45, pick2: 0 })).toEqual([{ a: 45, grade: 1 }])
    expect(petAttrsFromPicks({ pick1: 0, pick2: 0 })).toEqual([])
  })

  it('offers only the live rows of one grade', () => {
    const abilities = [
      { index: 45, grade: 1, name: '甲' },
      { index: 3, grade: 2, name: '乙' },
      { index: 0, grade: 0, name: '状态', state: true },
      { index: 71, grade: 2, name: '退役', excluded: true },
    ]
    expect(petSkillOptions(abilities, 1).map((row) => row.index)).toEqual([45])
    expect(petSkillOptions(abilities, 2).map((row) => row.index)).toEqual([3])
  })
})
