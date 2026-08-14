# Workarouhnd to ensure the build timestamp in the ESP app header is refreshed
# on every build. Delete esp_app_desc.c object file before each build (a
# ninja-side delete only doesn't work).
import os

def action_extensions(base_actions, project_path):
    def force_update_build_timestamp(ctx, global_args, tasks):
        if not ({t.name for t in tasks} & {'all', 'build', 'app'}):
            return
        build_dir = global_args.build_dir or os.path.join(project_path, 'build')
        obj = os.path.join(build_dir, 'esp-idf', 'esp_app_format', 'CMakeFiles',
                '__idf_esp_app_format.dir', 'esp_app_desc.c.obj')
        if os.path.exists(obj):
            os.remove(obj)

    return {
        'version': '1',
        'actions': {},
        'global_action_callbacks': [force_update_build_timestamp],
    }
