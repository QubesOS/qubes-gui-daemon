#ifndef __PACAT_CONTROL_H__
#define __PACAT_CONTROL_H__

#include <glib-object.h>
#include <dbus/dbus-glib.h>
#include "pacat-simple-vchan.h"

typedef struct PacatControl PacatControl;
typedef struct PacatControlClass PacatControlClass;

GType pacat_control_get_type (void);

struct PacatControl
{
  GObject parent;
  struct userdata *u;
};

struct PacatControlClass
{
  GObjectClass parent;
};

#define PACAT_CONTROL_TYPE              (pacat_control_get_type ())
#define PACAT_CONTROL(object)           (G_TYPE_CHECK_INSTANCE_CAST ((object), PACAT_CONTROL_TYPE, PacatControl))
#define PACAT_CONTROL_CLASS(klass)      (G_TYPE_CHECK_CLASS_CAST ((klass), PACAT_CONTROL_TYPE, PacatControlClass))
#define IS_PACAT_CONTROL(object)        (G_TYPE_CHECK_INSTANCE_TYPE ((object), PACAT_CONTROL_TYPE))
#define IS_PACAT_CONTROL_CLASS(klass)   (G_TYPE_CHECK_CLASS_TYPE ((klass), PACAT_CONTROL_TYPE))
#define PACAT_CONTROL_GET_CLASS(obj)    (G_TYPE_INSTANCE_GET_CLASS ((obj), PACAT_CONTROL_TYPE, PacatControlClass))

/*
typedef enum
{
  PACAT_CONTROL_ERROR_FOO,
  PACAT_CONTROL_ERROR_BAR,
  PACAT_CONTROL_ERROR_MULTI_WORD
} PacatControlError;

#define PACAT_CONTROL_ERROR (pacat_control_error_quark ())
#define PACAT_CONTROL_TYPE_ERROR (pacat_control_error_get_type ())

GQuark pacat_control_error_quark (void);
GType pacat_control_error_get_type (void);

*/

int dbus_init(struct userdata *u);

#endif
