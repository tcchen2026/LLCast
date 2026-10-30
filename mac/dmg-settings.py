# dmgbuild's settings for LLCast.dmg (tools/macos/build.sh): the app and an
# Applications link side by side.
#   dmgbuild -s mac/dmg-settings.py -D app=build/macos/dist/LLCast.app LLCast LLCast.dmg
files = [defines['app']]
symlinks = {'Applications': '/Applications'}
hide_extensions = ['LLCast.app']
icon = 'mac/LLCast.icns'
format = 'UDZO'
filesystem = 'HFS+'
window_rect = ((200, 120), (560, 340))
icon_size = 128
text_size = 13
icon_locations = {'LLCast.app': (140, 150), 'Applications': (420, 150)}
