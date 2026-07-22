/*
 * The session, as the device sees it.
 *
 * Nothing about the password lives in the browser: the device sets an
 * HttpOnly cookie that this code cannot read, and every answer about who is
 * logged in comes from asking the device rather than from remembering.
 */

export interface SessionState {
  /** The device requires a login at all. */
  required: boolean;
  authenticated: boolean;
  /** Logged in with the default password; nothing else may proceed. */
  mustChange: boolean;
  user: string;
}

async function postJson(url: string, body: unknown): Promise<Record<string, unknown>> {
  const res = await fetch(url, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  const parsed = (await res.json().catch(() => ({}))) as { error?: string };
  if (!res.ok) {
    throw new Error(parsed.error ?? `request failed (${res.status})`);
  }
  return parsed as Record<string, unknown>;
}

export async function loadSession(): Promise<SessionState> {
  const res = await fetch("/api/v1/auth/session", { cache: "no-store" });
  if (!res.ok) throw new Error(`the device did not answer (${res.status})`);
  return (await res.json()) as SessionState;
}

/** @returns true when the password in use is the default and must be changed */
export async function login(user: string, password: string): Promise<boolean> {
  const body = await postJson("/api/v1/auth/login", { user, password });
  return Boolean(body.mustChange);
}

export async function logout(): Promise<void> {
  await postJson("/api/v1/auth/logout", {});
}

/** Changing the password ends every session, including this one. */
export async function changePassword(current: string, next: string): Promise<void> {
  await postJson("/api/v1/auth/password", { current, next });
}
