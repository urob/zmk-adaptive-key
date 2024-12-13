# ZMK-ADAPTIVE-KEY

This module adds a `adaptive-key` behavior to ZMK. Some highlights compared to
existing alternatives:

- Works as a module without the need to patch ZMK.
- Configurable `dead-keys` property to turn any keycode into a dead key.
- Simple "inline" macro specification to bind behavior sequences.
- `min-prior-idle-ms` and `max-prior-idle` timeout properties that can vary by
  trigger.
- Correct handling of explicit modifiers.

## Usage

To load the module, add the following entries to `remotes` and `projects` in
`config/west.yml`.

```yaml
manifest:
  defaults:
    revision: v0.1 # version to use for this module and for ZMK
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: urob
      url-base: https://github.com/urob
  projects:
    - name: zmk
      remote: urob # or zmkfirmware, see comment below
      import: app/west.yml
    - name: zmk-leader-key
      remote: urob
  self:
    path: config
```

**Important:** The `zephyr` remote used by upstream ZMK currently contains a bug
that under certain circumstances causes the build to fail. You will need to
patch Zephyr if your build fails with an error message like:

```
ERROR: /behavior/leader-key POST_KERNEL 31 < /behaviors/foo POST_KERNEL 49
```

The simplest way to getting the patch is to use my `zmk` remote, as configured
in above manifest. This will automatically build against a patched version of
Zephyr. If you are building using Github Actions, you may need to clear your
cache (in the left sidebar on the `Actions` tab) for the changes to take effect.

## Configuration

An `adaptive-key` defines "trigger" conditions on the _last_ keycode pressed
prior to pressing the behavior. If any trigger condition matches, a behavior
bound to that trigger is invoked. If no trigger condition matches, a default
behavior is invoked.

### `trigger` properties

Triggers are defined as child nodes of an adapative-key instance and are checked
in order of their definition. Triggers have two _required properties_:

- **`trigger-keys`**: A list of keycodes that trigger the bindings.
- **`bindings`**: Behaviors bound to the trigger. If set to multiple behaviors
  they are invoked in sequence.

Additional conditions can be configured via _optional properties_:

- **`min-prior-idle-ms`**: Minimum time that must be elapsed since the last key
  press. Defaults to none.
- **`max-prior-idle-ms`**: Maximum time that must be elapsed since the last key
  press. Defaults to none.
- **`strict-modifiers`**: If true, modifiers must _exactly_ match the
  `trigger-keys`. Otherwise it suffices to _contain_ the `trigger-keys` (useful
  for case-sensitive bindings). Defaults to false.

### `adaptive-key` properties

Besides `trigger` child nodes, `adaptive-key` instances have the following
properties:

- **`bindings`** (required): The default behavior to invoke if no trigger
  condition is met. Can be `&none` to do nothing.
- **`dead-keys`**: A list of key codes that are converted to dead keys. Dead
  keys don't send a keycode when pressed the first time but are still considered
  as trigger condition. If pressed again, dead keys send their normal keycode.

## Examples

### Hands-down adaptive keys

```c
/ {
    behaviors {
        ak_h: ak_h {
            compatible = "zmk,behavior-adaptive-key";
            #binding-cells = <0>;
            bindings = <&kp H>;

            akt_ah { trigger-keys = <A>; max-prior-idle-ms = <300>; bindings = <&kp U>; };
            akt_uh { trigger-keys = <U>; max-prior-idle-ms = <300>; bindings = <&kp A>; };
            akt_eh { trigger-keys = <E>; max-prior-idle-ms = <300>; bindings = <&kp O>; };
        };

        ak_m: ak_m {
            compatible = "zmk,behavior-adaptive-key";
            #binding-cells = <0>;
            bindings = <&kp M>;

            akt_gm { trigger-keys = <G>; max-prior-idle-ms = <300>; bindings = <&kp L>; };
            akt_pm { trigger-keys = <P>; max-prior-idle-ms = <300>; bindings = <&kp L>; };
        };

        // And similarly for VP->VL, PV->LV, BT->BL, TB->LB

        ak_g: ak_g {
            compatible = "zmk,behavior-adaptive-key";
            #binding-cells = <0>;
            bindings = <&kp G>;

            // Binding two behaviors: JG->JPG
            akt_jg { trigger-keys = <J>; max-prior-idle-ms = <300>; bindings = <&kp P &kp G>; };
        };

    };
};
```

### Dead keys

```c
/ {
    behaviors {
        ak_e: ak_e {
            compatible = "zmk,behavior-adaptive-key";
            #binding-cells = <0>;
            bindings = <&kp E>;
            dead-keys = <GRAVE CARET APOS QUOTE>;

            grave { trigger-keys = <GRAVE>; bindings = <&fr_e_grave>; };
            acute { trigger-keys = <APOS>; bindings = <&fr_e_acute>; };
            circumflex { trigger-keys = <CARET>; bindings = <&fr_e_circumflex>; };
            diaeresis { trigger-keys = <QUOTE>; bindings = <&fr_e_diaeresis>; };
        };
    };
};
```

Note: the behavior bindings `&fr_e_grave` etc must be defined elsewhere (e.g.,
using the French
[language header](https://github.com/urob/zmk-helpers/tree/main#unicode-characters-and-language-collection)
from the `zmk-helpers` module).

Alternatively, "new" dead keycodes can be "created" by cannibalizing unused
keycode. For instance:

```c
#define DEAD1 F21
#define DEAD2 F22
#define DEAD3 F23
#define DEAD4 F24

/ {
    behaviors {
        ak_e: ak_e {
            compatible = "zmk,behavior-adaptive-key";
            #binding-cells = <0>;
            bindings = <&kp E>;
            dead-keys = <DEAD1 DEAD2 DEAD3 DEAD4>;

            grave { trigger-keys = <DEAD1>; bindings = <&fr_e_grave>; };
            acute { trigger-keys = <DEAD2>; bindings = <&fr_e_acute>; };
            circumflex { trigger-keys = <DEAD3>; bindings = <&fr_e_circumflex>; };
            diaeresis { trigger-keys = <DEAD4>; bindings = <&fr_e_diaeresis>; };
        };
    };
};
```

Note: While the keycodes used in this example are typically unused, they are
still [defined](https://zmk.dev/docs/keymaps/list-of-keycodes#f-keys). Making up
new _undefined_ keycodes is unsupported as their working hinges on the execution
order of this module, which cannot be configured by any supported means.

### Shift-repeat

```c
/ {
    behaviors {
        shift-repeat: shift-repeat {
            compatible = "zmk,behavior-adaptive-key";
            #binding-cells = <0>;
            bindings = <&sk LSHFT>;

            repeat {
                trigger-keys = <A B C D E F G H I J K L M N O P Q R S T U V W X Y Z>;
                bindings = <&key_repeat>;
                max-prior-idle-ms = <350>;
                strict-modifiers;
            };
        };
    };
};
```

This sets up a `shift-repeat` behavior that sends `&sk LSHFT` unless when
pressed within 0.35 seconds of any alpha key, in which case it sends
`&key_repeat`. Great for your homing thumb key!

## References

- The behavior idea is inspired by the
  [Hands Down](https://sites.google.com/alanreiser.com/handsdown/home#h.3fq4ywspvw1g)
  keyboard layout. The original ZMK
  [feature request](https://github.com/zmkfirmware/zmk/issues/1624) provides
  some further discussion.
- PR [#2042](https://github.com/zmkfirmware/zmk/pull/2042) provides an
  alternative implementation.
- My personal [zmk-config](https://github.com/urob/zmk-config) contains advanced
  usage examples.
