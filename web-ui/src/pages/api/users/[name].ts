import type { APIRoute } from "astro";
import { updateUserThresholds, renameUser } from "../../../lib/db";

export const prerender = false;

export const PATCH: APIRoute = async ({ params, request }) => {
  let name = decodeURIComponent(params.name!);
  const body = await request.json();

  if (typeof body.newName === "string" && body.newName.trim() !== name) {
    const result = renameUser(name, body.newName);
    if (!result.ok) {
      return new Response(JSON.stringify({ ok: false, error: result.error }), {
        status: 409,
        headers: { "Content-Type": "application/json" },
      });
    }
    name = body.newName.trim();
  }

  const thresholds: Record<string, [number, number]> = {};
  for (const key of ["hr", "perspiration", "skinTemp", "coreTemp", "respiration"]) {
    const low = Number(body[`${key}Low`]);
    const high = Number(body[`${key}High`]);
    if (Number.isFinite(low) && Number.isFinite(high) && low <= high) {
      thresholds[key] = [low, high];
    }
  }

  updateUserThresholds(name, thresholds);

  return new Response(JSON.stringify({ ok: true, name }), {
    status: 200,
    headers: { "Content-Type": "application/json" },
  });
};
