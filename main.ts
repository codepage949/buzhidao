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
  usp.set("source", (Deno.env.get("SOURCE") === "en") ? "en" : "zh-CN");
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

  console.log(Deno.env.get("PAPAGO_API_URL")!, resp.status);

  return await resp.json();
}

async function makeMessage(txts: string[]) {
  const joinedTxt = txts.join("\n\n");
  let pnins: string[];

  if (Deno.env.get("SOURCE")! === "ch") {
    pnins = pinyin(joinedTxt).flat().join(" ").split("\n\n");
  }
  
  const translateResult = await translate(joinedTxt);

  console.log("translateResult", translateResult);

  const translatedTxts = translateResult.translatedText.split("\n\n");
  let dict = "";

  for (const item of (translateResult.dict?.items) ?? []) {
    dict += `## ${item.entry.replace(/<.*?>/g, "")} ${(Deno.env.get("SOURCE")! === "ch") ? pinyin(item.entry.replace(/<.*?>/g, "")).flat().join(" ") : ""}\n`;

    for (const pos of item.pos) {
        for (const meaning of pos.meanings) {
            dict += `1. ${meaning.meaning.replace(/<.*?>/g, "")}\n`
        }
    }

    dict += "\n";
  }

  const output = [];

  for (let i = 0; i < txts.length; i++) {
    if (Deno.env.get("SOURCE")! === "ch") {
      output.push(
        [txts[i], pnins![i].trim(), translatedTxts[i]].join("\n"),
      );
    } else {
      output.push(
        [txts[i], translatedTxts[i]].join("\n"),
      );
    }
  }

  return output.join("\n\n") + "\n\n" + dict;
}

async function* infinity() {
  while (true) {
    yield;

    await new Promise((ok) => {
      setTimeout(ok, 0);
    });
  }
}

function isSourceLanguage(text: string) {
  return (Deno.env.get("SOURCE")! === "en") ? (/[a-zA-Z]/g).test(text): (/[\u4e00-\u9fa5]/g).test(text);
}

async function pollTgMessage() {
  let offset = 0;

  for await (const _ of infinity()) {
    const resp = await fetch(
      `https://api.telegram.org/bot${Deno.env.get(
        "BOT_TOKEN",
      )!}/getUpdates?offset=${offset}`,
      {
        method: "get",
      },
    );

    console.log(`https://api.telegram.org/bot${Deno.env.get(
      "BOT_TOKEN",
    )!}/getUpdates?offset=${offset}`, resp.status);

    const update = await resp.json();

    for (const result of update.result) {
      if (Deno.env.get("CHAT_ID")!) {
        const message = await makeMessage([result.message.text]);
        const resp = await fetch(
          `https://api.telegram.org/bot${Deno.env.get(
            "BOT_TOKEN",
          )!}/sendMessage`,
          {
            method: "post",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({
              chat_id: Deno.env.get("CHAT_ID")!,
              text: message,
            }),
          },
        );
  
        console.log(`https://api.telegram.org/bot${Deno.env.get(
          "BOT_TOKEN",
        )!}/sendMessage`, resp.status, await resp.json());
      } else {
        const resp = await fetch(
          `https://api.telegram.org/bot${Deno.env.get(
            "BOT_TOKEN",
          )!}/sendMessage`,
          {
            method: "post",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({
              chat_id: result.message.chat.id,
              text: result.message.chat.id,
            }),
          },
        );
  
        console.log(`https://api.telegram.org/bot${Deno.env.get(
          "BOT_TOKEN",
        )!}/sendMessage`, resp.status, await resp.json());
      }
      
      offset = result.update_id + 1;
    }

    await new Promise((ok) => {
      setTimeout(ok, 300);
    });
  }
}

function setKeyboardHook() {
  user32.symbols.SetWindowsHookExA(13, keyboardHook.pointer, 0, 0);
}

async function pumpWsMessage() {
  const msg = new Uint8Array(48);

  for await (const _ of infinity()) {
    user32.symbols.PeekMessageA(msg, 0, 0, 0, 1);

    await (async () => {
      if (isPrtScPressed) {
        isPrtScPressed = false;

        if (!isBusy) {
          isBusy = true;

          try {
            await new Promise((ok) => setTimeout(ok, 300));

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
              `${Deno.env.get("INFER_API_URL")!}/infer/${Deno.env.get("SOURCE")!}`,
              {
                method: "post",
                body: fd,
              },
            );
            
            console.log(`${Deno.env.get("INFER_API_URL")!}/infer/${Deno.env.get("SOURCE")!}`, resp.status);

            const detectionMap = new Map();
            const detections = await resp.json();

            for (const detection of detections) {
              const [leftUpper, , , leftBottom] = detection[0];
              const [txt] = detection[1];

              if (isSourceLanguage(txt)) {
                let near = null;

                for (const [candidateDetection] of detectionMap) {
                  const [x1, y1] = candidateDetection;
                  const [x2, y2] = leftUpper;
  
                  if ((x2 - x1) ** 2 <= parseInt(Deno.env.get("X_DELTA")!) && (y2 - y1) ** 2 <= parseInt(Deno.env.get("Y_DELTA")!)) {
                    near = candidateDetection;
  
                    break;
                  }
                }
  
                if (near) {
                  const oldTxt = detectionMap.get(near);
  
                  detectionMap.set(leftBottom, oldTxt + txt.trim());
                  detectionMap.delete(near);
                } else {
                  detectionMap.set(leftBottom, txt.trim());
                }
              }
            }

            let message = "no detections.";

            if (detectionMap.size > 0) {
              const txts = [];

              for (const [, txt] of detectionMap) {
                txts.push(txt);
              }

              message = await makeMessage(txts);
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
                  text: message,
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
  }
}

(function main() {
  pollTgMessage();
  setKeyboardHook();
  pumpWsMessage();
})();