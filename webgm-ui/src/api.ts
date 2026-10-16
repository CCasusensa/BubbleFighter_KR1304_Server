export class ApiError extends Error {
  status: number
  details: unknown

  constructor(message: string, status = 0, details: unknown = null) {
    super(message)
    this.name = 'ApiError'
    this.status = status
    this.details = details
  }
}

type ApiOptions = RequestInit & { signal?: AbortSignal }

export async function api<T>(path: string, options: ApiOptions = {}): Promise<T> {
  const { signal: upstreamSignal, ...requestOptions } = options
  const timeoutController = new AbortController()
  let timedOut = false
  const timeout = window.setTimeout(() => {
    timedOut = true
    timeoutController.abort()
  }, 15000)
  const abortFromCaller = () => timeoutController.abort()
  upstreamSignal?.addEventListener('abort', abortFromCaller, { once: true })
  try {
    const response = await fetch(path, {
      credentials: 'include',
      headers: { 'Content-Type': 'application/json', ...(requestOptions.headers || {}) },
      ...requestOptions,
      signal: timeoutController.signal,
    })
    const payload = await response.json().catch(() => ({}))
    if (response.status === 401) {
      window.dispatchEvent(new CustomEvent('webgm:unauthorized'))
    }
    if (!response.ok || payload.ok === false) {
      throw new ApiError(payload.error || `请求失败 (${response.status})`, response.status, payload)
    }
    return (payload.data ?? payload) as T
  } catch (error) {
    if (timedOut) throw new ApiError('请求超时，请稍后重试')
    throw error
  } finally {
    window.clearTimeout(timeout)
    upstreamSignal?.removeEventListener('abort', abortFromCaller)
  }
}

export function queryString(params: Record<string, string | number | boolean | undefined>): string {
  const query = new URLSearchParams()
  for (const [key, value] of Object.entries(params)) {
    if (value !== undefined && value !== '') query.set(key, String(value))
  }
  const encoded = query.toString()
  return encoded ? `?${encoded}` : ''
}
