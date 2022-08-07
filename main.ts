import { read } from "clipboard-image";
import { copy } from "streams";
import { config } from "dotenv";
import cryptoJs from "crypto-js";
import pinyin from "pinyin";

config({ export: true });

const user32 = Deno.dlopen(
  "user32.dll",
  {
    PeekMessageA: {
      parameters: ["pointer", "usize", "u32", "u32", "u32"],
      result: "i32",
      nonblocking: false,
    },
    SetWindowsHookExA: {
      parameters: ["i32", "function", "usize", "u32"],
      result: "i32",
      nonblocking: false,
    },
    CallNextHookEx: {
      parameters: ["usize", "i32", "usize", "usize"],
      result: "i32",
      nonblocking: false,
    },
  } as const,
);
const pressedMap = new Map<number, boolean>();
let isPrtScPressed = false;
let isBusy = false;
const keyboardHook = new Deno.UnsafeCallback(
  { parameters: ["i32", "usize", "usize"], result: "usize" } as const,
  (
    nCode: number,
    wParam: number | bigint,
    lParam: number | bigint,
  ): number | bigint => {
    const keyCode = (new Deno.UnsafePointerView(lParam as bigint)).getUint32();

    if (nCode === 0) {
      if (wParam === 256) {
        if (!pressedMap.get(keyCode)) {
          pressedMap.set(keyCode, true);
        }
      } else if (wParam === 257) {
        if (pressedMap.get(keyCode)) {
          pressedMap.set(keyCode, false);

          if (keyCode === 0x2C) {
            isPrtScPressed = true;
          }
        }
      }
    }

    return user32.symbols.CallNextHookEx(0, nCode, wParam, lParam);
  },
);

async function translate(text: string) {
  const uuid = crypto.randomUUID();
  const path = Deno.env.get("PAPAGO_API_URL");
  const timestamp = new Date().valueOf().toString();
  const papagoVersion = Deno.env.get("PAPAGO_VERSION")!;
  const auth = cryptoJs.enc.Base64.stringify(
    cryptoJs.HmacMD5(
      `${uuid}\n${path}\n${timestamp}`,
      papagoVersion,
    ),
  );

  const usp = new URLSearchParams();

  usp.set("deviceId", uuid);
  usp.set("locale", "en");
  usp.set("dict", "true");
  usp.set("dictDisplay", "30");
  usp.set("honorific", "false");
  usp.set("instant", "false");
  usp.set("paging", "false");
  usp.set("source", "zh-CN");
  usp.set("target", "ko");
  usp.set("text", text);

  const resp = await fetch(Deno.env.get("PAPAGO_API_URL")!, {
    method: "post",
    headers: new Headers({
      "authorization": `PPG ${uuid}:${auth}`,
      "timestamp": timestamp,
    }),
    body: usp,
  });

  return await resp.json();
}

(() => {
  user32.symbols.SetWindowsHookExA(13, keyboardHook.pointer, 0, 0);

  const msg = new Uint8Array(48);

  const loop = () => {
    user32.symbols.PeekMessageA(msg, 0, 0, 0, 1);

    (async () => {
      if (isPrtScPressed) {
        isPrtScPressed = false;

        if (!isBusy) {
          isBusy = true;

          try {
            await new Promise((ok) => setTimeout(ok, 500));

            const img = await read();

            await copy(
              img,
              await Deno.open("img.bmp", { create: true, write: true }),
            );
            await Deno.run({ cmd: ["vips", "copy", "img.bmp", "img.png"] })
              .status();
            await Deno.run({ cmd: ["vipsthumbnail", "-s", "1920x1080>", "-f", "output.png", "img.png"] })
              .status();

            let fd = new FormData();

            fd.set(
              "file",
              new File([await Deno.readFile("output.png")], "output.png"),
            );

            let resp = await fetch(
              `${Deno.env.get("INFER_API_URL")!}/infer`,
              {
                method: "post",
                body: fd,
              },
            );
            
            console.log(`${Deno.env.get("INFER_API_URL")!}/infer`, resp.status);

            let txt = "";

            while (true) {
              resp = await fetch(
                `${Deno.env.get("INFER_API_URL")!}/get`,
                {
                  method: "get",
                },
              );

              txt = await resp.text();

              console.log(`${Deno.env.get("INFER_API_URL")!}/get`, resp.status, txt);

              if (txt.length > 0) {
                break;
              }

              await new Promise((ok) => {
                setTimeout(ok, 1000);
              });
            }

            const json = [];

            for (const found of txt.matchAll(/\[\[\[.+?\]\], \('(.+?)'.+\)\]\n/g)) {
              json.push(found[1]);
            }

            const text = json.join("\n\n");
            const pnin = pinyin(text).flat().join(" ");
            const result = await translate(text);
            let dict = "";

            for (const item of (result.dict?.items) ?? []) {
              dict += `## ${item.entry.replace(/<.*?>/g, "")} ${pinyin(item.entry.replace(/<.*?>/g, "")).flat().join(" ")}\n`;

              for (const pos of item.pos) {
                  for (const meaning of pos.meanings) {
                      dict += `1. ${meaning.meaning}\n`
                  }
              }

              dict += "\n";
            }

            const translated = result.translatedText;
            const newText = text.split("\n\n");
            const newPnin = pnin.split("\n\n");
            const newTranslated = translated.split("\n\n");
            const output = [];

            for (let i = 0; i < newText.length; i++) {
              output.push(
                [newText[i], newPnin[i], newTranslated[i]].join("\n"),
              );
            }

            fd = new FormData();

            fd.set("chat_id", Deno.env.get("CHAT_ID")!);
            fd.set(
              "photo",
              new File([await Deno.readFile("output.png")], "output.png"),
            );

            resp = await fetch(
              `https://api.telegram.org/bot${Deno.env.get(
                "BOT_TOKEN",
              )!}/sendPhoto`,
              { method: "post", body: fd },
            );

            console.log(`https://api.telegram.org/bot${Deno.env.get(
              "BOT_TOKEN",
            )!}/sendPhoto`, resp.status, await resp.json());

            resp = await fetch(
              `https://api.telegram.org/bot${Deno.env.get(
                "BOT_TOKEN",
              )!}/sendMessage`,
              {
                method: "post",
                headers: { "content-type": "application/json" },
                body: JSON.stringify({
                  chat_id: Deno.env.get("CHAT_ID")!,
                  text: output.join("\n\n") + dict,
                }),
              },
            );

            console.log(`https://api.telegram.org/bot${Deno.env.get(
              "BOT_TOKEN",
            )!}/sendMessage`, resp.status, await resp.json());
          } finally {
            isBusy = false;
          }
        }
      }
    })();

    setTimeout(loop, 0);
  };

  setTimeout(loop, 0);
})();
