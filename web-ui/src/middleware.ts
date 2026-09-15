import { defineMiddleware } from "astro:middleware";
// Side-effect import: starts the server-side sensor recording loop once,
// the first time any request comes in — see src/lib/sensorLoop.ts.
import "./lib/sensorLoop";

export const onRequest = defineMiddleware((_context, next) => next());
